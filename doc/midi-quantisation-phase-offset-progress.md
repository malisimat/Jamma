# MIDI Quantisation Phase Offset Progress

## What Has Been Done

This branch has landed the first structural slice for shared MIDI quantisation phase offsets.

- The MIDI snap math now supports a phase origin instead of always snapping against loop-local zero.
- `MidiQuantisationSettings` now carries a take-local `PhaseOffsetSamps` field.
- `LoopTake` now keeps local phase offset, inherited phase offset, and a transport-derived natural phase separate, then resolves them into one effective value before publishing to owned `MidiLoop` instances.
- `Quantisation` now has a global phase offset entry point, and `Station` now has global and station phase offset storage plus propagation into its takes.
- `QuantisationLoopTakeVisual` and `QuantisationModel` now carry and render the resolved phase offset, so the quantisation overlay rotates with the same phase concept used by MIDI snapping.
- Introduced MIDI phase drag functionality in Scene, allowing Ctrl-Shift drag to adjust global phase offset.
- Updated Station to apply MIDI quantisation phase offsets when adding takes.
- Focused native tests were added for positive and negative phase offsets, the local-only pack/unpack contract, and transport-anchor alignment across different take start positions.

## Current State

The code now supports Ctrl-shift drag to affect global quantisation phase offset.  This offset is aligned across stations and takes.
Note there is still no active user-facing control for station offset, so it should remain zero unless that later becomes intentional.

## Proposed Stages

### Stage 1: Better UX with overlay drag handles for quant/edit adjustment

Goal: the system should provide intuitive responsive control of global and looptake prop-edits through mouse drag on popup overlay handles

- Ctrl opens the edit overlay and becomes the only entrypoint for quant-edit drags.
- If nothing is hovered, show only the global phase handle.
- If a Station is hovered, expose station-level handles; if a LoopTake is hovered, expose looptake-level handles; if a Loop is hovered, expose loop-level handles.
- If something is selected, apply the same handle set to all selected objects, with object type determined by the current depth mode.
- If something is hovered, show all available buttons tiled in a single column and color them distinctly by action.
- Handle actions: global phase = drag left/right, LoopTake fraction = drag up/down, LoopTake relative playback offset = drag left/right.
- Reserve the overlay for future modes like scratch, pitch, stretch, and other per-object edits.
- Place the overlay near the mouse, but flip it above, below, left, or right when the cursor is near a screen edge.
- Keep selection separate from quant editing (use Shift modifier instead of Ctrl modifier), ideally on a distinct marquee gesture so it never fights the overlay chord.

Work:


Deliverable:



### Stage 2: Persistence and session semantics

Goal: the new phase state survives save/load and behaves like other editable timing data.

Work:

- Add persistence for global phase offset.
- Add persistence for station phase offset.
- Add persistence for take-local phase offset.
- Decide backward compatibility behavior for sessions created before this feature.
- Decide whether phase edits should participate in undo/redo.

Deliverable:

- Saved sessions reopen with the same quantisation grid alignment.

### Stage 3: Visual polish and correctness passes

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

### Stage 4: Integration tests and regression coverage

Goal: prove the user-facing behavior rather than only the math.

Work:

- Add tests for inherited phase composition: global + station + take.
- Add tests for station propagation into all owned takes.
- Add tests for `LoopTake` resolved quantisation publication into `MidiLoop`.
- Add tests for overlay visual generation using resolved phase.
- Add at least one higher-level test for two takes with different local zeroes aligning to the same shared grid.

Deliverable:

- Regression coverage for the actual feature contract.

## Validation Already Run

- `Build Tests (Debug x64)` succeeded
