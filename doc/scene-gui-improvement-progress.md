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
   baseline/feature performance comparison: in progress. An opt-in native
   render harness now loads the production UI resource list and Inter fonts,
   creates an owned hidden WGL context, and draws the real Scene-created
   settings panels plus a populated production HUD into an FBO. GPU readback
   on the AMD Radeon 880M (OpenGL 4.6 compatibility context) produced seven
   reviewed captures: Timing/MIDI, closing/collapsed, 320 x 180 during and after
   animation, and restored 1280 x 720. Exact pixels from an ordinary texture/font
   control drawn after the panels match across fade/collapse/resize. The harness
   settles the HUD's existing cable reveal fade before comparison, initializes
   each tree, rejects pre-existing GL work, and restores global deletion-thread
   ownership after teardown. Driver-dependent evidence stays opt-in; ordinary
   native runs skip it. Run from the repository root with
   `JAMMA_RENDER_EVIDENCE_DIR` set to the output directory and
   `--gtest_filter=GuiRenderEvidence.*` (read local tasks first).
   Bright-background visual review caught a real palette defect: the old
   `rounded_but` texture's centre alpha is only 62/255, washing out an intended
   80% graphite fill. Settings frames, trigger rail and source identity popup
   now reuse `rounded_but_on`, whose opaque centre preserves that fill. GPU
   pixels assert the actual interior alpha composition over the bright clear
   colour; text remains legible and edge gradients retain existing artwork.
   Incremental Debug library/native/app builds passed; 179 focused checks,
   including the GPU evidence pass, passed (877 ms). The threading audit found
   no hot-path lock/wait additions; its only flagged new ownership/lock state
   belongs to the test context's setup/teardown. Captures and logs are local
   ignored artifacts under `test/JammaLib_Tests/bin/x64/Debug/`.
   A second opt-in GPU pass now covers buttons, toggles, textboxes, numeric
   inputs and dropdowns at 17/25/36/43 px heights under a translated parent.
   Readback asserts visible glyph pixels in all 20 controls and in each of three
   popup rows. Reviewed captures include ascenders/descenders/digits, focused
   selection/caret after resizing through all four heights, and dropdown rows
   with a long clipped label. Every focus-induced RGB change lies inside the
   baseline-derived caret/underline bands. Nested 0.5 x 0.5 scopes produce the
   expected 25% blend on an isolated textured centre; an ordinary texture/font
   control drawn afterward remains pixel-identical to its unfaded reference.
   Dropdown lists now use the existing opaque-centre rounded skin with an
   explicit configurable graphite tint, improving row readability over other
   controls. Focused review found no material resource/lifecycle defect.
   Incremental Debug library/native/app builds passed, all 180 focused checks
   including both GPU passes passed (2676 ms), and the threading audit found
   no hot-path lock/wait or shared-state additions.
   A third isolated opt-in pass now constructs a plugin-free Scene with eight
   stations, sixteen triggers and an empty LoopTake, then uses production
   `Window::Create(SW_HIDE)`, all 90 resources, `Window::Render`, default-framebuffer
   readback and `Swap`. Four reviewed captures show expanded/closing panels,
   native 480 x 320 resize, and restoration to 1000 x 650 over the real skybox
   and station geometry. The window stays hidden. Native `SetWindowPos` must
   propagate through WM_SIZE (no direct Resize fallback); client, Window and
   Scene sizes are checked, with exact transition preservation before the next
   render tick. Edge-handle glyph regions remain nonempty after resizing.
   Window-level panel input is consumed without scene selection; actual All/Off
   radio clicks change the LoopTake's resolved MIDI quantisation policy.
   A scope guard releases Window resources, shuts down Scene and restores prior
   GL deletion ownership on assertion exits too. This test has a second opt-in
   `JAMMA_WINDOW_RENDER_EVIDENCE` and runs in an isolated process with a 30-second
   external timeout because production Create may show a modal driver error.
   Focused review found no remaining material issue. Incremental Debug native
   build passed; 183 ordinary checks passed (310 ms), with three GPU checks
   skipped as intended. An isolated combined pass ran all 186 checks including
   all GPU/production-window evidence and passed (4481 ms), with an empty GL
   error log. The audit's only new ownership/lock state is test-only cleanup.
   An ignored source archive of code baseline
   `815dfd309b82d7c22d1e91eb5ccad02dbc188eca` is prepared under `bin/gui-baseline`
   for the same-scene comparison; no performance conclusions have been drawn.
   The baseline library and native target now build successfully. A shared
   unchanged benchmark header (SHA-256
   `936395AB5E76503D44C616AA4B5250241327486DACD1F15C1C2034E702ED9E59`)
   verifies eight stations, sixteen loaded mono loops with unique take/loop
   identities, and twenty-four triggers. Both revisions use the same quiet
   generated PCM sidecar, viewport, resources, Debug configuration, AMD GPU and
   OpenGL 4.0 core context. The runner reads local tasks, owns a hidden isolated
   test process, drains logs asynchronously, rejects test failure/skip/stale CSV,
   and checks 600 fresh rows. Its one-second timeout path was exercised and
   correctly stopped only the owned process. Final baseline and feature runs
   were sequential after compilation, passed (25907/25401 ms), and produced
   empty GL error logs. Raw timings/metadata/logs are local ignored artifacts
   under each revision's `test/JammaLib_Tests/bin/x64/Debug/gui-benchmark/`.
   Each workload has 120 measured frames after warmup. Dispatch, Render plus
   glFinish, swap, and total iteration time are separate. Cable reveal remains
   held; panel-edge/wheel/pointer scenarios inject common input but do not prove
   equivalent control effects or successful cable drags. Native resize checks
   actual dimensions. Swap interval 1 remains enabled and substantially affects
   total time. Preliminary Render+glFinish p50/p95/p99 (milliseconds):

   | Workload | Baseline | Feature |
   | --- | --- | --- |
   | Idle with cables | 14.866 / 16.504 / 18.361 | 14.471 / 16.273 / 19.558 |
   | Panel edge input | 13.834 / 16.240 / 17.865 | 13.050 / 15.925 / 23.527 |
   | Wheel input | 14.352 / 16.107 / 16.840 | 14.271 / 16.572 / 22.797 |
   | Native resize | 15.118 / 19.974 / 25.253 | 14.611 / 19.750 / 24.252 |
   | Pointer input | 15.425 / 16.231 / 16.551 | 14.740 / 15.990 / 19.704 |

   This single pair is preparation evidence, with ordering/noise and unverified
   input effects; it establishes no performance budget or live-audio result.
   Playback was not started and underruns are unmeasured. Full comparison still
   needs verified interactions under playback and repeated/interleaved runs.
   Focused benchmark/runner review found no material issue after timing/label
   corrections. The normal suite passed 183 checks (216 ms), with all four
   driver/benchmark checks skipped as intended; both opt-in benchmark runs
   separately passed. ASIO device/rate/buffer details have been requested for
   the live gate; only registered driver names (MAYA22USB/ASIO4ALL), not hardware
   connectivity, have been inspected.
   Real foreground desktop interaction/maximize/restore, populated loop playback
   and baseline frame-time/underrun comparison
   remain unverified. The computer-use skill was read, but this session exposes
   no required `node_repl` tool; no desktop automation was attempted.
   A subsequent production-window scroll check exposed two input ownership
   defects: wheel events could retain a capture despite having no matching
   release, and zero-button moves cleared settings captures but left ordinary
   HUD captures alive. Scene now excludes mouse-wheel events from pointer
   capture/focus changes and cancels ordinary widget ownership with its pressed
   state on native capture loss. Existing held gestures keep their owners;
   keyboard focus survives cancellation. HUD cancellation uses the existing
   scene mutex protecting HUD input and tree replacement. The threading audit
   flags this added lock by filename; manual review confirms it is in UI
   `Scene::OnAction(TouchMoveAction)`, with no callback, tick, or render-lock
   scope changes. Focused independent review found no material issue.
   Two regression checks verify that the actual settings handle receives its
   press/release and collapses after a wheel event or abandoned GUI capture.
   The production-window check now reveals cables, scrolls the real trigger
   rail, verifies its offset changes, renders its end offset, then clicks the
   real Timing controls and verifies quantisation policy changes. Visually
   inspected local captures `production-window-scrolled.bmp` and
   `production-window-scroll-end.bmp` show capture/station cable continuations
   at the rail boundaries over the actual scene. These remain hidden-window,
   no-playback checks. Incremental native and app builds passed; the focused
   suite passed 188 checks (3721 ms), with only the separately enabled benchmark
   skipped and an empty GPU error log. The preliminary timings above predate
   these input fixes and are historical preparation evidence.
   The production-window fixture now loads one real mono audio loop into each
   of eight stations and restores a completed MIDI note-on/off stream before
   resource initialization. It verifies loop membership/length, opens the real
   MIDI editor, waits for readiness using a bounded wall-clock deadline, and
   renders it with expanded settings. Modified settings presses/releases and
   panel wheels are consumed; the special engaged-editor HUD path scrolls the
   real trigger list, and modified All/Off clicks change the take's resolved
   quantisation state. Editor pitch range/visible rows and MIDI edit revision,
   event count and timestamps remain unchanged, covering wheel fallthrough as
   well as note edits. Closing verifies zero morph, closed state and cleared
   target. Actual captures `production-window-expanded.bmp`,
   `production-window-midi-editor.bmp` and
   `production-window-editor-restored.bmp` were visually inspected: loaded
   models/editor draw beneath settings and restore correctly. This verifies
   populated rendering and editor input, not hardware playback. Focused review
   strengthened deadline waits and wheel-state assertions; the final suite
   passed 188 checks (5207 ms), with the separate benchmark skipped and no GPU
   errors. The incremental native build passed. Inspection also identified a
   remaining plan gap: Window clamps zero client dimensions to one and retains
   old Scene layout when minimized. Zero-viewport handling is the next slice.
7. Final requirement audit and HTML completion report: pending.

## Local build setup

The worktree initially lacked `vcpkg_installed`. Its dependency tree was copied
from the main checkout into this worktree; the local task definitions were not
changed. The test output also needed the dependency Debug DLLs copied from that
tree in addition to the build's existing Google Test DLL copy step.
