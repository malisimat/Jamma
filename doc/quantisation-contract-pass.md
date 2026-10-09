# Quantisation contract and gap pass

Baseline: `8604af2`, branch `feature/quantisation-enhancements`. Research findings
are entry points, not reproduced live failures.

## Contracts chosen before implementation

- One completed local take is one performance, including MIDI-only and
  multichannel takes. Remote, recording/incomplete and pre-reclock takes do not
  establish the current local master.
- A sole local performance may reinterpret integer beats. Beat-count half ties
  round upward; audio grain floors to an integer sample count, and logical audio
  length is its exact product with grain count. Retain the physical buffer/tail
  through the existing `Loop::Play` boundary path. MIDI source lengths/events
  remain intact; a grid change must not trim or stretch MIDI.
- Additional takes and remote authority freeze construction geometry. Select
  the nearest permitted straight/triplet base division with smaller-count ties.
  A tap-selected base grid composes once with the existing per-take fraction;
  offsets compose once with inherited station/global offsets. Preserve enable
  state. Remote beat/BPI and local grain remain distinct references.
- Use the existing off-callback MIDI publication path. Quantised onset moves
  carry the note duration; existing loop-end handling applies. Visible occupancy
  is the union of projected note coverage and may merge on coarser grids. Raw
  notes and edit revisions do not change merely because resolution changes.
  Returning to prior geometry recovers the pattern if no notes were edited.
- Capture selection first at the active depth, then relevant hover, then global,
  on Ctrl press. Keep the capture through drag. Loop scope must remain on its
  owning loop rather than silently editing siblings.
- Physical Space down performs one timestamped tap and holds the grid; repeat
  does nothing, up releases. Text entry wins. Ctrl state updates even when the
  editor consumes the action. Space and gesture holds compose independently.
- Timeout is strictly greater than three seconds. Zero is a valid first sample;
  invalid/non-increasing inputs restart smoothing without changing accepted geometry.

## Confirmed gaps and research references

- `Quantiser::HandleTapTempo` requires an audio loop for automatic master
  discovery and resize; `LoopTake::VisualLoopLengthSamps` already supports MIDI.
- Frozen taps only store `_activeGridDivisions`; no playback/editor consumer.
- `SetMidiGrain` clears remote grid state and includes remote stations.
- `Scene::OnAction(KeyAction)` routes editor/focus before Ctrl/Space and taps on
  keyup. `QuantiserController::OnCtrlModifierChanged` holds the grid for Ctrl.
- Controller captures hover/depth but resolves selected targets later. Loop
  targets are promoted to takes; cancellation does not cover the controller.
- `CtrlHandleOverlay::Draw` draws both flat coloured quads without captions,
  drag isolation or the requested panel treatment.
- Fraction enum and drag order currently share six persisted ordinals. Keep
  ordinals 0–5 and append triplets; introduce explicit presentation order.
- `QuantisationLoopTakeVisual` lacks the effective remote/grid descriptor even
  though editor/playback use `ResolvedMidiQuantisation`.

## Validation boundaries

Native helpers alone cannot establish UI routing, rendered agreement, live
audio continuity or NINJAM integration. Final evidence must distinguish tests,
builds, actual interaction/diagnostic traces and anything still unverified.
