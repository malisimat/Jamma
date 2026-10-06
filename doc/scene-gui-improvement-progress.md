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
   implemented. Removed virtual minimum dimensions, nominal input-width cap,
   rail overhang and unexplained vertical offsets. Viewports use signed,
   clamped client geometry. Audio/MIDI space is allocated by populated source
   counts; empty categories reserve no viewport. Cards resize in place between
   80 and 160 px with full logical row extents when the minimum overflows.
   Labels reserve meter space, sockets follow card centers, and scroll offsets
   are preserved/clamped. Trigger cards stay 100 px tall; only viewport height
   changes. Header/footer controls hide when their real frames cannot fit.
   Long labels use resolved glyph advances and retained full text for ellipsis.
   Clicking a source opens a retained, horizontally scrollable identity popup;
   availability is structured input and an explicit Offline label, not parsed
   from device names or conveyed solely through colour. Popup state is UI-owned;
   job-side rig rebuilds do not edit it. Existing published rig inputs and
   Scene's guarded HUD rebuild path are retained. Fonts remain existing
   resources, with requested label heights preserved after font selection.
   Pointer cancellation clears callback press state, and scroll/HUD resource
   teardown now covers their independently owned content and popup trees.
   Native tests cover widths, tiny/zero viewports, single-row/socket alignment,
   widget reuse, scroll clamping, measured ellipsis and identity popup capture/
   resize/cancel. Final focused code/lifecycle reviews found no material defect.
   Incremental Debug library, test and app builds passed, and all 153 focused
   GUI/cable/layout/Scene tests passed (157 ms). Runtime/visual evidence
   and the smallest supported interactive window size remain slice 6 work.
4. Production settings pages, command adapters, focus/capture cleanup and
   explicit overlay draw/input priority: implemented. The retained top panel
   contains selection depth only. The bottom-left panel starts on Timing and
   switches between real MIDI and Timing pages. Off/Mixed/All, channel override,
   local phase offset and CLICK use the existing Scene owners through fixed
   command relays; tree initialization can renumber controls without changing
   their commands. Future Audio/Session/Groups composition slots have no fake
   actions. Standalone radio draw/resource/input/receiver/hover paths are removed.
   Panels draw above scene/editor/HUD and below popups. New gestures inside their
   bounds precede editor/modifier/wheel handling; existing captures retain their
   streams. Settings releases are consumed, including collapse and page-hide
   cancellation. Windows zero-button cancellation clears capture and drag state.
   Focus changes and page/collapse transitions explicitly validate pending edits,
   stop numeric drags and close only descendant-owned popups. Owner updates retain
   focused text/caret until validation; invalid/partial/nonfinite values revert
   to the last applied value. Retained controls resize and recover from tiny/zero
   viewports, and pages keep their own scroll offsets. Panel/page/relay/focus/
   capture state is UI-owned and tears down with the Scene GUI; no audio callback
   state or synchronization changes were introduced. The threading audit found
   no hot-path lock/wait additions, and callback-owned bodies are unchanged.
   Final focused code reviews found no material defect; pointer-dispatched Scene
   collapse/reopen and cancellation checks close the remaining coverage gap.
   Incremental Debug library, native test and app builds passed. All 165 focused
   GUI/cable/layout/Scene tests passed (161 ms).
   Animation, shared opacity/palette and live visual confirmation remain slices
   5 and 6 work.
5. Stable text geometry, scoped opacity, shared palette and frame-time motion:
   in progress: requested label frames survive font resolution, and measured
   ellipsis is available to HUD labels. Panels now slide upward/leftward with one
   normalized transition driving smoothstep position and alpha over 220 ms.
   Reversal starts at the current value; resize preserves progress and recomputes
   real anchors. Window advances motion once per UI frame from the monotonic
   wall clock before deferred hover. Resume intervals clamp at 50 ms; motion
   updates only presentation position/visibility, with no page-layout or cable
   rebuild. Idle transitions return without invalidating hover. Closing gates
   page/tab input immediately while retaining the visible body blocker; persistent
   edge handles remain opaque, in-window and independent of body motion.
   UI opacity scopes multiply and restore without heap-backed stacks. Textures,
   fonts, flat decorations, meters, cables and Ctrl handles declare/apply the
   effective alpha, including ordinary draws at 1. Per-image panel fill opacity
   (80%) is separate from transition/text opacity. Graphite/control/edge presets
   are shared by panels and related HUD/control construction; the panel has a
   restrained one-pixel accent. The render audit also removed a per-label copy
   of the owning OpenGL context; GPU contexts are now explicitly noncopyable.
   Global MIDI quantisation was corrected to the plan's initial Timing page,
   alongside phase offset and CLICK. Native tests cover nesting/restoration,
   ordinary uniform defaults, reversal/resume/resize, closing content gates,
   and hiding during numeric capture with consumed release. Focused motion and
   render reviews found no material defect. Incremental Debug library, test and
   app builds passed; all 171 focused GUI/cable/layout/Scene checks passed
   (354 ms), and the threading audit found no hot-path lock/wait additions.
   Vertical glyph/caret geometry is now implemented: fonts expose cached scaled
   ascent/descent/line-gap metrics, and labels place a baseline inside a stable
   frame with explicit baseline/bottom/center/top alignment. Control labels and
   headers center consistently; standalone baseline placement remains available
   and popup body rows retain explicit top alignment. Text clipping composes
   with parent/window scissors. Compact text frames clamp vertical padding to
   reserve the minimum available font; zero-size frames stay empty. Related
   button/toggle/text/numeric/dropdown defaults share 36 px height and 8 px
   padding. Source cards use the same 36 px height with two explicit 16 px text
   rows; trigger cards remain 100 px. Textbox caret/selection bands and pointer
   X lookup use the label's clamped frame and currently selected font, including
   after resize. External textbox/dropdown labels have weak presentation parents
   for correct global clipping, plus explicit init/release ownership for their
   labels, list rows and adornments. Pure tests cover alignment, odd/tiny/zero
   frames, caret/selection clipping, common control defaults and resizing; CPU
   glyph-bound checks use the actual Inter font at all available sizes. Focused
   render/lifecycle reviews found no material defect. Incremental Debug library,
   test and app builds passed, all 178 focused checks passed (247 ms), and the
   threading audit found no hot-path lock/wait additions. Shader-backed visual
   confirmation and final palette tuning remain slice 6 work.
6. Integration review, app build, runtime/visual checks during playback and
   baseline/feature performance comparison: pending.
7. Final requirement audit and HTML completion report: pending.

## Local build setup

The worktree initially lacked `vcpkg_installed`. Its dependency tree was copied
from the main checkout into this worktree; the local task definitions were not
changed. The test output also needed the dependency Debug DLLs copied from that
tree in addition to the build's existing Google Test DLL copy step.
