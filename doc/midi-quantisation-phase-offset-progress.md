# MIDI Quantisation Phase Offset Progress

## What Has Been Done

This branch has landed the first structural slice for shared MIDI quantisation phase offsets.

- The MIDI snap math now supports a phase origin instead of always snapping against loop-local zero.
- `MidiQuantisationSettings` now carries a take-local `PhaseOffsetSamps` field.
- `LoopTake` now keeps local phase offset, inherited phase offset, and a transport-derived natural phase separate, then resolves them into one effective value before publishing to owned `MidiLoop` instances.
- `Quantisation` now has a global phase offset entry point, and `Station` now has global and station phase offset storage plus propagation into its takes.
- `QuantisationLoopTakeVisual` and `QuantisationModel` now carry and render the resolved phase offset, so the quantisation overlay rotates with the same phase concept used by MIDI snapping.
- Focused native tests were added for positive and negative phase offsets, the local-only pack/unpack contract, and transport-anchor alignment across different take start positions.

## Current State

The code now supports this composition model:

`effective phase = transport-derived natural phase + global offset + station offset + take-local offset`

That effective phase is used in two places:

- MIDI quantised playback event publication
- Quantisation overlay rotation

This means the codebase now has a clean place to put shared phase alignment, and the visual layer is no longer hard-wired to a purely local loop interpretation. Note there is still no active user-facing control for station offset, so it should remain zero unless that later becomes intentional.

## Important Limitations

This is not the full feature yet.

### 1. Transport-derived alignment is now in place, but scope editing is still missing

The current implementation now derives each take's natural phase from a shared transport anchor captured at record / overdub start.

What is still missing is the UI path that lets the user edit the global phase offset directly:

- enabling `Ctrl-Shift` drag to act on the global quantisation phase
- converting drag distance into a musically sane offset step
- keeping the overlay and models live during the drag
- committing or cancelling the gesture cleanly

So the transport anchor exists now, but the interactive adjustment surface is still the next chunk of work.

### 2. No GUI payload or persistence wiring yet

The new phase offset is intentionally not packed into the existing packed quantisation payload, because there are no spare bits in the current format.

That means the following are still missing:

- GUI payload transport for phase offset edits
- jam file persistence for global / station / take phase offsets
- load / save compatibility rules for old sessions
- undo / redo semantics if phase dragging should be undoable

### 3. Overlay semantics are only partially evolved

The overlay gates now use the resolved phase offset, which is good, but there are still visual questions left to settle:

- whether the station overlay should represent the grid only, or grid plus playhead phase
- whether the overlay column/origin marker should remain at loop origin or follow quantisation origin
- whether MIDI note models also need a separate quantisation-origin visual marker instead of only inheriting loop rotation
- how to best represent the quantisation fractions/subdivisions (currently only x1 is displayed, not x0.5, x0.25, etc) for each LoopTake

### 4. No end-to-end integration coverage yet

The branch now has focused unit coverage for snap math and transport-anchor composition, but it still does not prove the full user-facing target behavior:

- two takes with different local zeroes
- two takes in different stations with different local zeroes
- one shared timing seed
- one shared phase origin
- both quantising to the same grid lines

That still needs an integration-style test or a targeted engine test.

## Proposed Stages

### Stage 1: Finish the timing anchor model

Goal: compute a natural, shared phase for each take from the same timing basis.

Work:

- Verify the current transport-anchor formula against the actual station / clock behavior under record and overdub.
- Decide whether the transport anchor should stay on record / overdub start, or move to a higher-level shared timing identity later.
- Keep manual offsets (i.e. the global phase shift) additive on top of that natural phase instead of replacing it.

Deliverable:

- A deterministic `natural phase + global + station + take` composition model.

### Stage 2: Wire scope-specific editing

Goal: let the user edit phase offsets, which acts on global offset regardless of the current active depth mode.

Work:

- Wire `Ctrl-Shift` left/right drag to modify the global offset.
- Normalize drag deltas in a musically sensible way.
- Recommended default: drag in pixels->milliseconds, with configurable sensitivity
- Ensure updates push through `Quantisation`, `Station`, and `LoopTake` live during drag.

Deliverable:

- Interactive global phase editing with live overlay feedback.

### Stage 3: Make automatic alignment actually work across stations

Goal: the system should align takes from different stations without manual compensation when they share the same timing basis.

Work:

- Apply the transport-derived natural phase to all stations from the same global timing root.
- Verify that station-local overrides remain additive rather than replacing inherited phase.
- Confirm that changing the global phase shifts all stations consistently.
- Confirm that station phase shifts only that station's takes.
- Confirm that take phase shifts only the selected take.

Deliverable:

- Real cross-station shared-grid behavior, not just per-station plumbing.

### Stage 4: Persistence and session semantics

Goal: the new phase state survives save/load and behaves like other editable timing data.

Work:

- Add persistence for global phase offset.
- Add persistence for station phase offset.
- Add persistence for take-local phase offset.
- Decide backward compatibility behavior for sessions created before this feature.
- Decide whether phase edits should participate in undo/redo.

Deliverable:

- Saved sessions reopen with the same quantisation grid alignment.

### Stage 5: Visual polish and correctness passes

Goal: make the visuals clearly communicate the new model.

Work:

- Adjust the QuantisationModel rotation for each LoopTake that represents its offset, so they all line up visually (global grid).
- Add a new visual like 'banded rows' that displays the current fraction/subdivisions of each LoopTake, like a set of quads arranged circumferentially around a cylinder (aligned to vertical), facing outwards with gaps in between where the quad is equal width to the gap, and their widths indicate the subdivision size (1/32 would be very narrow).  Height to match its associated LoopTake height.
- Decide whether overlay origin markers should show loop origin, quantisation origin, or both.
- Decide whether the MIDI model should visually expose quantisation-origin shifts separately from playhead rotation.
- Verify that overlay rotation remains intuitive when phase shifts are large or negative.
- Audit render-side use of phase values for precision and normalization edge cases.

Deliverable:

- Clear visual communication of quantisation origin versus loop playback state.  Clear visual display of subdivisions.

### Stage 6: Integration tests and regression coverage

Goal: prove the user-facing behavior rather than only the math.

Work:

- Add tests for inherited phase composition: global + station + take.
- Add tests for station propagation into all owned takes.
- Add tests for `LoopTake` resolved quantisation publication into `MidiLoop`.
- Add tests for overlay visual generation using resolved phase.
- Add at least one higher-level test for two takes with different local zeroes aligning to the same shared grid.

Deliverable:

- Regression coverage for the actual feature contract.

## Recommended Next Move

The next highest-value step is **Stage 2**.

The transport-derived anchor is now in place, so the feature needs a user-facing way to move the global phase around. That makes the gesture path the best next slice: it will exercise the full propagation path without waiting on persistence and file-format work.

## Validation Already Run

- `Build Tests (Debug x64)` succeeded
- `JammaLib_Tests.exe --gtest_filter=LoopTakeMidiQuantisation.*:MidiLoopQuantisation.*:MidiQuantisation.*` passed
