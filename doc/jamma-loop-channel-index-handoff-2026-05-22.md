# Handoff: Plan to Fix Loop Index vs ADC Channel Assumption

Date: 2026-05-22
Repo: Jamma-vst3

## Session Goal
Create an implementation plan to remove the incorrect assumption that a Loop's index equals the ADC input channel index.

Target behavior:
- Trigger/record routing may map any ADC channel(s) to loop(s).
- Recorded loops remain sequentially indexed in a LoopTake (0..N-1).
- Playback/mix/FX in LoopTake continues to operate over sequential loop buffers.

## Existing Behavior (Verified)
1. Station fans ADC/MONITOR/BOUNCE writes to each LoopTake via channel index:
   - JammaLib/src/engine/Station.cpp (OnBlockWriteChannel)
2. LoopTake currently resolves record source channels by directly indexing `_loops[channel]`:
   - JammaLib/src/engine/LoopTake.cpp (`_InputChannel`)
3. LoopTake::AddLoop currently wires each Loop mixer output to `chan` (ADC channel) and also stores `LoopParams::Channel = chan`:
   - JammaLib/src/engine/LoopTake.cpp (`AddLoop(unsigned int chan, ...)`)
4. LoopTake playback aggregate assumes per-loop sequential scratch/buffer/mixer arrays:
   - JammaLib/src/engine/LoopTake.cpp (`WriteBlock`, `_audioBuffers[i]`, `_audioMixers[i]`)

Consequence:
- If recording channels are non-contiguous or remapped (e.g., ADC 2 and 5), record-path dispatch and playback aggregation can diverge.

## Root Cause
The code uses one integer (`chan`) to represent two different concepts:
- Record input source channel (ADC channel ID)
- Internal loop slot/output index inside LoopTake (sequential loop index)

These must be modeled separately.

## Proposed Refactor Strategy
Separate "record input mapping" from "loop slot index".

### Data Model
In LoopTake, add explicit per-loop mapping from loop slot -> ADC source channel:
- Front buffer mapping: `_loopInputChannels`
- Back buffer mapping: `_backLoopInputChannels`

Keep loop slot indices sequential (0..N-1), and use those for:
- Loop->LoopTake mixer wiring for playback aggregation
- LoopTake `_audioBuffers` / `_audioMixers` indexing
- Gui routing indices

### Record Path
Do not rely on `_InputChannel(channel, recordSource)` for record fanout by ADC index.

Implement explicit record fanout in LoopTake for ADC/MONITOR/BOUNCE sources:
- For incoming source channel C, forward write to each loop slot i where `_loopInputChannels[i] == C`.
- For non-record sources, preserve existing behavior.

This can be done by overriding `OnBlockWriteChannel` in LoopTake (preferred) or by changing `_InputChannel` contract + caller behavior. Overriding `OnBlockWriteChannel` is less invasive and supports future N:1 or 1:N mappings.

### Playback Path
When creating loops during Record/Overdub:
- Keep source ADC channel in mapping vectors.
- Wire each Loop mixer output to the loop slot index (not ADC channel):
  `wire.Channels = { loopSlotIndex }`.

That preserves LoopTake internal aggregation assumptions and VST processing over contiguous loop channels.

## File-Level Edit Plan

1. JammaLib/src/engine/LoopTake.h
- Add mapping members:
  - `std::vector<unsigned int> _loopInputChannels;`
  - `std::vector<unsigned int> _backLoopInputChannels;`
- Add helper(s), e.g.:
  - `_LoopInputChannelsForCurrentBuffer()` accessors
  - optional lookup helper for source->loop slots
- Add override declaration for `OnBlockWriteChannel(...)`.

2. JammaLib/src/engine/LoopTake.cpp
- Constructor/member-init: initialize new mapping vectors.
- `AddLoop(unsigned int chan, ...)`:
  - Compute `loopSlotIndex = _backLoops.size()` before push.
  - Configure Loop mixer wire output to `loopSlotIndex`.
  - Store source `chan` in `_backLoopInputChannels` aligned to loop slot.
- `_CommitChanges()`:
  - Flip `_loopInputChannels = _backLoopInputChannels` with loops/audio buffers/mixers.
- `Ditch()` and any clear/reset paths:
  - clear mapping vectors consistently with loops.
- Add `OnBlockWriteChannel(...)` override:
  - For ADC/MONITOR/BOUNCE: fan out to mapped loops by source channel.
  - For LOOPS/MIXER: keep existing base routing to `_audioBuffers`.
- `_InputChannel(...)`:
  - keep only for LOOPS/MIXER path (or maintain safe behavior for record sources but do not depend on index equality).

3. JammaLib/src/engine/Loop.h and JammaLib/src/engine/Loop.cpp
- Decide and document `LoopParams::Channel` semantics after refactor:
  - Recommended: internal loop slot/output index, not ADC source channel.
- Ensure any UI/layout usage (`LoopChannel()`) remains coherent with sequential slots.

4. (Optional UI semantics check) JammaLib/src/engine/LoopTake.cpp
- `_ArrangeChildren()` currently uses `loop->LoopChannel()` for X placement.
- Confirm this still behaves correctly once `LoopChannel` is slot index.

5. Tests: test/JammaLib.Tests/src/audio/AudioFlow_Tests.cpp
- Add regression test for non-sequential input channels, e.g. Record({2,5}) with station bus >= 6:
  - Write distinct data on channels 2 and 5.
  - Verify both loops record.
  - Verify playback appears via contiguous loop slots (0/1) before take-level routing.

6. Tests: test/JammaLib.Tests/src/engine/FlipBuffer_Tests.cpp
- Extend LoopTake flip-buffer tests to assert mapping vectors flip/clear with loop vectors.

7. Tests: (Optional) new focused LoopTake routing test file
- Add direct unit test for `OnBlockWriteChannel` fanout by mapped source channels.

## Suggested Implementation Order
1. Add mapping vectors + commit/clear plumbing.
2. Change AddLoop wiring to slot index and persist input mapping.
3. Add LoopTake::OnBlockWriteChannel record fanout override.
4. Update/adjust `_InputChannel` to avoid hidden coupling.
5. Add regression tests.
6. Build and run targeted tests.

## Build/Test Commands (Targeted)
Use project-targeted builds and explicit SolutionDir (per repo guidance).

- Build tests project:
  - test/JammaLib.Tests/JammaLib.Tests.vcxproj (Debug x64)
- Run tests exe:
  - test/JammaLib.Tests/bin/x64/Debug/JammaLib.Tests.exe
- Optional focused filter:
  - --gtest_filter="AudioFlow.*:LoopTakeFlipBuffer.*"

## Risks / Watchouts
- Record fanout semantics: if multiple loops map to one ADC channel, behavior should be intentional (fanout additive) and covered by tests.
- Existing UI/router assumptions may still expect index=channel in some places; verify GuiRack route initialization and slider wiring after refactor.
- Ensure no RT-unsafe operations are introduced in audio callback path (no allocations/locks/exceptions).

## References
- Existing architecture notes: doc/vst-channel-routing.html
- Related code docs/comments in:
  - JammaLib/src/engine/LoopTake.cpp
  - JammaLib/src/engine/Station.cpp
  - JammaLib/src/engine/Trigger.cpp

## Suggested Skills
- Explore: quickly confirm all remaining index/channel coupling sites before editing.
- handoff: produce a follow-up handoff after implementation/testing.
- agent-council (optional): if there are competing designs for preserving LoopParams::Channel semantics.
