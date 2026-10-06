# Scene GUI implementation progress

The requirements in [scene-gui-improvement-plan.md](scene-gui-improvement-plan.md)
remain the completion contract. This file records implementation evidence, not
a replacement scope. The feature is not complete.

## Design checkpoint

- Overlay coordinates use the existing bottom-origin pixel system. Scroll
  content rectangles are half-open signed bounds, excluding padding and bars.
  Effective bounds intersect ancestor scroll clips and the actual window.
  Cable presentation will use HUD-local bounds; only visible real sockets may
  be snap targets. Invalid station projections retain logical routes without
  manufacturing a screen direction.
- Scene retains settings policy and synchronization. Panel-owned receivers
  will map widget actions to explicit legacy commands because tree
  initialization renumbers child indices. One tree owns each widget's drawing,
  resources and input. Selection-depth camera behavior remains coupled as today.
- Settings start expanded, Timing is the initial page, and reopen handles stay
  at the real window edges. Panel transitions and composition are UI-owned;
  existing router, quantiser and audio-boundary command paths retain ownership.
- Panels will explicitly arbitrate before scene gestures when no pointer
  capture is active. Capture release/cancel and edit finalization must be
  resolved before hiding a page. Popups remain above overlays.
- Audio device switching, Session actions, Groups and numeric BPM remain the
  plan's follow-ups. No fake controls will be exposed.

## Slices and evidence

1. Shared scroll clipping and hidden-parent input gates: implemented; focused
   review identified and corrected HUD socket visibility using raw viewport
   bounds. Added tests for both scrollbar orientations, empty/tiny bounds,
   nested scroll transforms, window intersections and hidden/disabled stacks.
   Socket hit/snap bounds also reject pointer positions outside their content
   clip, including snap hysteresis. Saved snap identity is resolved against
   current socket geometry so scrolling/resizing cannot retain a hidden snap.
   Final focused review found no concrete defects in this slice. Incremental
   Debug x64 library, native-test and app builds passed. All 139 tests selected by
   `Gui*.*:Rect2d.*:CableInteractionTests.*` passed (145 ms total). This proves
   the tested clipping geometry and input gates, not runtime rendering or
   offscreen connection preservation. Those remain subsequent slices.
2. Cable boundary presentation, valid station projection and shared Bezier
   rendering/hit geometry: in progress. Render/hit control points now share one
   construction path and the same 24-point line strip as the shader. Capture
   and station curved-hit/chord-miss tests passed; all 140 focused GUI/cable
   tests passed (111 ms). Incremental Debug library/test/app builds passed, and
   focused independent review found no concrete defect. Boundary continuation
   geometry and projection validity are now implemented. Station anchors use
   optional floating-point projection with explicit finite/front/depth checks.
   Presented endpoints clamp to effective scroll/window bounds while retaining
   actual anchors and original revision/route handles. Source and trigger
   visibility no longer gates connected route construction. Empty/invalid
   geometry omits presentation while leaving the routing graph intact.
   Continuation notches are decorative and socket snapping stays restricted
   to real visible sockets. Fans contract near visible boundaries and spread
   along clipped edges continuously. Existing cable drags refresh their fixed
   displayed endpoint after geometry changes. Shader uploads/draws batch at
   16 curves to respect the 64-control-point uniform capacity.
   Geometry tests cover projection rejection, all boundary edges/corners,
   continuity and marker hit rejection. Real HUD tests cover both ends clipped,
   scroll restoration, invalid/empty presentation, 41 routes and unavailable
   input identity. All 147 focused GUI/cable/Scene tests passed (116 ms).
   Final independent review found no remaining concrete defect in this slice.
   Native correctness evidence is recorded; runtime/visual and performance
   checks remain required in slice 6.
3. Actual HUD viewport layout, adaptive source cards and trigger viewport:
   pending.
4. Production settings pages, command adapters, focus/capture cleanup and
   explicit overlay draw/input priority: pending.
5. Stable text geometry, scoped opacity, shared palette and frame-time motion:
   pending.
6. Integration review, app build, runtime/visual checks during playback and
   baseline/feature performance comparison: pending.
7. Final requirement audit and HTML completion report: pending.

## Local build setup

The worktree initially lacked `vcpkg_installed`. Its dependency tree was copied
from the main checkout into this worktree; the local task definitions were not
changed. The test output also needed the dependency Debug DLLs copied from that
tree in addition to the build's existing Google Test DLL copy step.
