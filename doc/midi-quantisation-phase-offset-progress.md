# MIDI Quantisation Phase Offset Progress

## What Has Been Done

This branch has landed the first structural slice for shared MIDI quantisation phase offsets and the supporting visuals/tests around it.

- The MIDI snap math now supports an explicit phase origin instead of always snapping against loop-local zero.
- `MidiQuantisationSettings` now carries a take-local `PhaseOffsetSamps` field.
- `LoopTake` now keeps local phase offset, inherited phase offset, and a transport-derived natural phase separate, then resolves them into one effective value before publishing to owned `MidiLoop` instances.
- `Quantisation` now has a global phase offset entry point, and `Station` now has global and station phase offset storage plus propagation into its takes.
- `QuantisationLoopTakeVisual` and `QuantisationModel` now carry and render the resolved phase offset, so the quantisation overlay rotates with the same phase concept used by MIDI snapping.
- `Scene` now supports Ctrl-Shift drag for adjusting quantisation phase at the active edit depth: background/global, station, LoopTake, and Loop-owned take targets. Hovered targets and selected peers are updated together.
- `Station` now applies MIDI quantisation phase offsets when adding takes.
- Save/load now persists global, station, and take-local phase offsets, with old sessions defaulting missing fields to zero.
- Focused native tests cover positive and negative phase offsets, the local-only pack/unpack contract, transport-anchor alignment across different take start positions, station-to-take propagation, resolved phase publication into `MidiLoop`, resolved visual publication, depth-aware scene drag routing, and persistence defaults/round-trips.

## Current State

The engine now carries a full phase chain of global, station, and take-local offsets. The rendered quantisation overlay uses the resolved value, and sessions reopen with saved grid alignment.

The main remaining gap is polish: the current interaction is depth-aware Ctrl-Shift drag rather than a visible popup handle picker with screen-edge-aware placement. Visual banded subdivision rows, explicit origin markers, and undo/redo participation still need a design pass.

## Next Stages

### Stage 1: Better UX with overlay drag handles for quant/edit adjustment

Goal: provide intuitive, responsive control of global and LoopTake prop-edits through mouse drag on popup overlay handles.

Work:

- Define the overlay state model for hovered, selected, and depth-mode-specific handles.
- Wire the handle routing so each edit target resolves to the correct global, station, LoopTake, or Loop phase field.
- Implement screen-edge-aware placement and a stable column layout for the handle picker.
- Keep the interaction contract explicit for future per-object edit modes.

Deliverable:

- Mouse-driven overlay editing for quantisation and LoopTake timing that is discoverable, depth-aware, and does not conflict with selection.

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

- Adjust the QuantisationModel rotation for each LoopTake that represents its offset, so they all line up visually on the shared grid.
- Add a new visual like banded rows that displays the current fraction/subdivisions of each LoopTake, with widths indicating subdivision size.
- Decide whether overlay origin markers should show loop origin, quantisation origin, or both.
- Decide whether the MIDI model should visually expose quantisation-origin shifts separately from playhead rotation.
- Verify that overlay rotation remains intuitive when phase shifts are large or negative.
- Audit render-side use of phase values for precision and normalization edge cases.

Deliverable:

- Clear visual communication of quantisation origin versus loop playback state, plus a clear visual display of subdivisions.

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
