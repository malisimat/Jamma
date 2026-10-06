# Scene GUI improvement: current state

## Status

The original implementation is committed. Follow-up changes from user testing on 6 October 2026 are also committed individually.

## User-testing refinements

- Audio and MIDI source categories shrink to occupied card widths and align together to the right, with consistent gaps and existing 80?160px card limits and scrolling.
- Default panel controls use muted amber.
- Selection/settings handles use the existing GuiRack arrow textures, with horizontal orientation for settings and state-dependent images.
- Panel borders use 8px screen margins; selection/settings handles overlap the panel rather than reserving an extra strip.
- A bottom-right status panel contains routing status, editor feedback/mode and Jamma version. The trigger rail ends above it. The Edit Loop button is removed; keyboard editing remains available.
- HUD inputs have a translucent rounded background and an inline Inputs label that hides when space is limited.

Incremental Debug x64 builds of JammaLib and Jamma pass after these refinements. No new tests were run. The status panel narrows to the space beside settings; very compact clients can hide its text.

These changes require another visual review in the app, especially compact-window overlap and expander appearance. The earlier verification results below refer to the original implementation; no new runtime or playback acceptance is claimed.

## Implemented

| Area | Current state |
| --- | --- |
| Production panels | Demo content removed. Top panel contains selection depth; bottom-left has MIDI and Timing pages. Both start expanded, with Timing selected and persistent reopen handles. |
| Existing settings | MIDI channel override 0–16, global Off/Mixed/All quantisation, local phase offset −1..1 loops and CLICK retain their existing owners and semantics. Shortcut and owner feedback are synchronized. |
| Input ownership | Stable command bindings survive tree initialization and child additions. Inactive pages do not participate in layout/input. Panels block scene gestures, wheels and modifier clicks in normal/editor modes. |
| Edit and popup cleanup | Closing/switching finalizes numeric edits, resolves invalid text, cancels captures and clears affected transient state. Owned popups close without dismissing unrelated prompts; confirmation popups fit the viewport. |
| Responsive HUD | Audio/MIDI source cards remain single-row, adapt from 80px to 160px and scroll. Populated categories each receive room for a minimum card when space allows. Trigger cards stay 100px high; rail/header/footer/Add/Delete follow the real viewport. |
| Cable continuity | Logical routes survive clipping. Presented endpoints resolve to scroll/window boundaries with decorative markers. Drawing and body hits share curve geometry; socket visibility, route identity, revisions and drag cancellation are preserved. |
| Text and style | Stable text frames use font metrics, consistent control sizing and aligned caret/selection geometry. Long identities are inspectable. Graphite fills, borders and scoped shader opacity are applied. |
| Motion and resize | Panels use a 220ms wall-clock transition for position/fade, preserving progress on reversal/resize and resolving hover during motion. Zero clients skip drawing/swapping/input while retaining valid backing resources. |
| Persistence and future pages | No panel state persistence was added. Audio, Session and Groups composition slots remain available without exposing unwired actions. |

## Verification state

- The user has manually run the app and accepted the current results. Their additional feedback is recorded and implemented above.
- Native and dependent app builds pass. The selected regression suite passes 281 checks covering GUI/geometry, owner bindings, MIDI/timing boundaries and actual GPU rendering.
- Native window checks cover resize, actual maximize/minimize/restore, stationary-pointer hover, normal/editor input priority, popup sizing and compact selection/settings controls.
- Component checks cover trigger Add/Delete/Cancel, snapshot projection, cable-body reconnect, cancellation and stale revisions. They do not establish adoption during hardware playback.
- GPU checks cover populated Scene/editor rendering, control families, text/caret/selection, cable continuations and nested opacity restoration.
- Focused reviews have no unresolved material findings; threading audits pass.
- The shared populated-scene benchmark produces frame distributions and verifies trigger scrolling and native resize in both baseline and feature revisions. Its current runs do not start playback or measure underruns, and its panel/pointer scenarios do not establish animation or cable drag.

## Known limitation

The tested compact interactive profile is a 400×240 client, using panel handles and scrolling to expose controls sequentially. Below that size, rendering and handles degrade safely, but labels and controls can become cramped or clipped; full usability is not established.

## Outstanding

1. Visually review the committed user-testing refinements across large and compact windows.
2. Complete detailed playback verification of relocated settings and applied-state feedback, routing adoption, cable reconnect/cancel and camera/drag interactions, popup/edit/tab behavior, and compact-window readability.
3. Measure repeated/interleaved baseline and feature frame-time distributions during playback, animation, scrolling, resize and cable drag, with the same populated scene and ASIO device/rate/buffer/channel configuration. Record actual driver underrun observations. The current ASIO resync flags are not a reliable independent count of hardware underruns or overruns.
4. Complete the original requirement audit and final HTML completion report if that deliverable is still wanted. Neither has been completed.

## Outside the original feature scope

Device-selection/switching UI, interactive session load/save plumbing, selection/mute groups and numeric BPM adjustment remain separate future work. No new Tap button was added.
