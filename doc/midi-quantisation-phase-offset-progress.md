# MIDI Quantisation Phase Offset Progress

## What Has Been Done

This branch has landed the first structural slice for shared MIDI quantisation phase offsets and the supporting visuals/tests around it.

- The MIDI snap math now supports an explicit phase origin instead of always snapping against loop-local zero.
- `MidiQuantisationSettings` now carries a take-local `PhaseOffsetSamps` field.
- `LoopTake` now keeps local phase offset, inherited phase offset, and a transport-derived natural phase separate, then resolves them into one effective value before publishing to owned `MidiLoop` instances.
- `Quantisation` now has a global phase offset entry point, and `Station` now has global and station phase offset storage plus propagation into its takes.
- `QuantisationLoopTakeVisual` and `QuantisationModel` now carry and render the resolved phase offset, so the quantisation overlay rotates with the same phase concept used by MIDI snapping.
- `Scene` now supports Ctrl drag for adjusting quantisation phase at the active edit depth: background/global, station, LoopTake, and Loop-owned take targets. Hovered targets and selected peers are updated together.
- The quantisation overlay is Ctrl-momentary, fades smoothly on release, renders specular-shaded subdivision bands, and shows hue-controlled phase-origin handles aligned to each take's resolved phase.
- `Station` now applies MIDI quantisation phase offsets when adding takes.
- Save/load now persists global, station, and take-local phase offsets, with old sessions defaulting missing fields to zero.
- Focused native tests cover positive and negative phase offsets, the local-only pack/unpack contract, transport-anchor alignment across different take start positions, station-to-take propagation, resolved phase publication into `MidiLoop`, resolved visual publication, depth-aware scene drag routing, and persistence defaults/round-trips.

## Current State

The engine now carries a full phase chain of global, station, and take-local offsets. The rendered quantisation overlay uses the resolved value, and sessions reopen with saved grid alignment.

The main remaining gap is optional UX expansion: the current interaction is depth-aware Ctrl drag with a momentary/fading overlay rather than a separate popup picker. Banded subdivision rows and phase-origin handles are rendered in the overlay; undo participation is out of scope for this pass.

## Next Stages

### Stage 1: Better UX with overlay drag handles for quant/edit adjustment

Goal: provide intuitive, responsive control of global and LoopTake prop-edits through mouse drag on popup overlay handles.

Work:

- Keep the Ctrl-momentary overlay contract explicit for future per-object edit modes.
- Optionally design a separate screen-edge-aware picker if the inline phase-origin handles are not discoverable enough in play.

Deliverable:

- Mouse-driven overlay editing for quantisation and LoopTake timing that is discoverable, depth-aware, and does not conflict with selection.

### Stage 2: Persistence and session semantics

Goal: the new phase state survives save/load and behaves like other editable timing data.

Work:

- Add persistence for global phase offset.
- Add persistence for station phase offset.
- Add persistence for take-local phase offset.
- Decide backward compatibility behavior for sessions created before this feature.
- Undo/redo participation is intentionally out of scope for this visual pass.

Deliverable:

- Saved sessions reopen with the same quantisation grid alignment.

### Stage 3: Visual polish and correctness passes

Goal: make the visuals clearly communicate the new model.

Work:

- Keep QuantisationModel rotation aligned to each LoopTake's resolved offset so the shared grid lines up visually.
- Keep specular-shaded banded rows visible enough to show the current fraction/subdivisions of each LoopTake.
- Keep phase-origin handles readable as quantisation-origin markers; loop-origin markers can be added separately if needed.
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
