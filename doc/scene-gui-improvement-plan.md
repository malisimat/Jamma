# Scene GUI improvement plan

## Current status

The original implementation and the subsequent user-testing refinements are committed. The refinements cover right-aligned input cards, warmer control colours, image expanders, 8px outer margins, a bottom-right status panel and a translucent input background with inline label.

The original scope below remains the reference for the initial implementation. See [current progress](scene-gui-improvement-progress.md) for follow-up changes and remaining visual/playback verification.

## Original intended outcome

- Expanded settings panels overlay the scene; collapsed panels retain labelled, clickable reopen handles.
- The top panel contains only selection depth. Input routing remains in the separate GuiHud.
- The bottom-left demo panel is replaced by real MIDI and Timing pages.
- Existing settings retain their owners, actions, validation, shortcut feedback and timing semantics.
- Source cards remain in one horizontal row per category, adapt their widths and scroll when necessary.
- Trigger cards retain their 100px height while their viewport follows the window.
- Connected cables remain visible when valid endpoints are outside the window or clipped by scroll panels.
- Text is aligned consistently, with translucent grey panels, restrained borders and time-based slide/fade motion.
- The existing GUI library and assets are used. No new UI toolkit or docking framework is required.
- Panel positions, sizes, open state and selected page are not persisted. Existing JAM/rig persistence remains unchanged.

The intended defaults are both panels expanded, Timing selected, the top panel sliding upward and the bottom-left panel sliding leftward. Reopen handles remain on the window edges.

## Production pages and scope

| Page | Intended controls |
| --- | --- |
| Selection | Existing selection-depth radio, retaining its camera/view behavior |
| MIDI | Existing channel override: 0 means unchanged; 1–16 force a MIDI channel |
| Timing | Global MIDI quantisation Off/Mixed/All; local phase offset in loop fractions; existing CLICK toggle |
| Audio, Session, Groups | Composition slots for future working actions |

Global MIDI quantisation remains distinct from grid division, tempo and transport synchronisation. Mixed preserves local take settings; global changes must not overwrite their local grids. Local phase offset retains the existing absolute-value/delta application and audio-boundary handoff. CLICK retains its existing meaning.

Audio/MIDI device selectors, live device switching, interactive session load/save wiring, selection/mute group assignment and recall, and numeric BPM adjustment are outside this feature. A Tap button was optional and could only use the existing timestamp/timing-policy path. Future controls require real owner/action support; no demo or pretend-functional controls belong in these pages.

## Ownership, layout and input

- Scene remains the presentation/wiring orchestrator; engine, timing, device and session policy retain their existing owners. Preserve the ownership boundaries in [glossary.md](glossary.md).
- Use actual client dimensions and bottom-origin overlay coordinates. Anchor selection to the top and settings to the bottom-left, accounting for HUD bounds and persistent handles.
- Bound expanded panels to available space and scroll page content. Keep header/tabs independent of content scrolling; retain/clamp each page's scroll offset.
- Use signed, clamped geometry. Tiny/zero clients have no negative dimensions or fabricated off-window viewports; skip empty draw and hit regions.
- At usable sizes prioritise reopen handles, header/tabs, then scrolling content. Define a smallest supported interactive profile through visual review; arbitrary tiny windows cannot keep every control usable.
- Draw scene/editor content below settings overlays and popups above them. Visible backgrounds block pointer input, including wheels and modifier clicks, before scene gestures in normal and loop-editor modes.
- Existing captures retain their gesture until release/cancel. Closing content rejects new presses immediately while the visible background and reopen handle remain interactive.
- One widget tree owns drawing, resources, hover, focus and input. Command identity is independent of child indexing and remains valid after initialization and later child additions.
- Retain inactive pages without placing them in layout, draw or input traversal.
- Before closing/switching pages, finalize valid numeric edits, revert invalid text, cancel pointer drags, clear affected focus/hover/capture and close only popups owned by that subtree. Consume terminating releases so they cannot become scene actions.
- Synchronize displayed values with real owners, including shortcut-driven changes. No audio callback reads the widget tree.

## Responsive source cards and triggers

- Keep separate horizontal audio/MIDI scroll parents. Allocate only populated categories and use the available area after margins and the trigger rail.
- Compute card widths from available space, padding, gaps and count, with an approximately 80px floor and a modest preferred maximum. Preserve single-row layout and scroll full logical content when it overflows.
- Resize card widgets in place where practical. Update labels, sockets, meters, content extents, scrollbar metrics and cable geometry together.
- Measure truncation using the resolved font. Keep full device identity inspectable and availability understandable through text/icons as well as colour.
- Preserve valid scroll offsets and clamp after resize/rebuild. Replacing content requires interaction cleanup and explicit offset preservation.
- Anchor the trigger rail to the actual right edge and height. Preserve 100px cards, spacing, header/footer, routing status, Add/Delete/Cancel and reveal-newest behavior; short windows reduce the viewport rather than compressing cards.

## Cable presentation and interaction

- Keep logical route identity, true socket/anchor position and presented endpoint separate. Clipping does not remove a connection; missing/unavailable targets remain distinct from clipped targets.
- Share the true content-visible rectangle between drawing, hit tests, sockets and cable resolution. Include padding, scrollbar exclusion, scroll transforms, ancestor clips and window bounds in one coordinate space.
- Visible endpoints use their true positions. Source-row continuations terminate at left/right boundaries, trigger continuations at top/bottom boundaries, and valid off-screen station anchors at the scene boundary. Resolve both ends when both are clipped.
- Preserve fanning within bounds and transition continuously back to true sockets when they enter view. Do not independently ease endpoints away from visible sockets.
- Validate finite projection and the camera front/near-plane convention before division. Bound floating-point projections before converting to integer pixels. Invalid/behind-camera anchors retain routes but omit their presentation until projection is valid.
- Draw resolved curves outside content scissors but inside the window. Preserve reveal/hover/editor policy and use subdued continuation markers.
- Markers are decorative, never socket hit/snap targets. Real sockets are interactive only within their visible clips.
- Cable-body hits match the rendered curve and preserve original route/fixed-end identity. Preserve pending-edit availability and revision checks; replacement graphs cancel stale drags.
- Recompute when routing, scrolling, camera or viewport geometry changes. Empty effective clips retain routes without presentation/hit targets.
- Settings occlude cables and block grabs underneath them; they do not reroute cables to settings-panel edges.

## Text, palette and motion

- Preserve requested control/content frames when selecting a font. Resolve vertical placement from actual ascent/descent, bearings and baseline.
- Use consistent geometry and preferred sizes across buttons, toggles, radios, dropdowns, numeric/text inputs, popup rows, headers and HUD labels. Align carets and selections to the same text baseline/frame.
- Give source/trigger labels explicit one-line/two-line layouts. Retain intentional top-aligned multiline labels.
- Use centralized style values and existing rounded/nine-patch assets. Suggested treatment is smoked graphite at 70–85% resting opacity, angled/chamfered cool-grey borders and clipped-corner accents. Use restrained cyan for focus/pages, amber for timing, red for mute/error and grey for unavailable state, preserving semantic cable and MIDI colours.
- One normalized transition drives position and opacity over approximately 180–250ms. Reversal starts from current progress; resizing recomputes anchors without restarting motion. Decorative motion must not delay actions.
- Advance once per UI frame from a monotonic wall clock before hover and drawing. Invalidate hover for changed bounds, clamp long resume intervals and use current layout for input. Never drive motion from audio time.
- Scoped opacity multiplies parent and local values, then restores prior state. Apply it once to existing alpha in plain/tinted textures, fonts and decorations; ordinary draws explicitly use opacity 1.
- Keep resting panel fill opacity separate from transition opacity, retaining readable text over bright scene content.
- Reuse geometry caches and capacities. Idle panels and unrelated fades must not rebuild routes or continually invalidate layout. Avoid decorative per-frame framebuffer allocation, font rasterization, shader compilation or new blur passes.

## Real-time constraints

This is presentation work. Preserve existing audio/MIDI behavior, remote-follow restrictions and publication/application boundaries described in [loop-alignment-and-ninjam-sync.md](loop-alignment-and-ninjam-sync.md) and [realtime-audio.md](realtime-audio.md).

Panel state stays UI-owned. Engine values use existing commands and published immutable snapshots. No widget work, allocations, locks, device I/O or font work is added to audio callbacks, and render locks must not expand around dialogs or file/device I/O.

## Original verification and completion criteria

- Resize large/small/large, maximize/restore and during animation; verify real anchors, reachable handles and safe tiny/zero geometry.
- Cover zero/one/many sources, both categories, long/unavailable names, widths crossing the 80px floor, both scroll extremes and resize after scrolling.
- Cover empty/long trigger lists, Add/Delete/Cancel, reveal-newest, fixed card heights, usable headers/footers and scrolling after resize.
- Cover all station/scroll edges and corners, partial/both-end/nested clipping, invalid projection, camera movement, restored true endpoints and unchanged route identity.
- Verify reveal/hover, rendered-body grabs, reconnect, cancel, stale-revision rejection and unavailable-source behavior in normal/editor modes.
- Verify panel reversal, editing during tab changes, popup placement, focus/capture loss, hidden-page rejection, click-through protection and restart defaults without persistence.
- Verify every relocated control reaches its existing owner after tree initialization/child additions, including applied feedback, shortcuts, channel limits, quantisation states, phase units and remote-follow semantics.
- Inspect all control families after font/resource initialization and resize, including ascenders/descenders, digits, long names, caret/selection, clipping and popup rows.
- Verify nested opacity, scope restoration and ordinary draws after fades through actual shader-backed rendering.
- Compare the same populated scene before/after under playback, animation, scrolling, resize and cable drag. Record frame-time distributions and actual audio underruns; derive numerical budgets from the baseline.
- Review affected ownership/threading and audio hot paths, with focused reviews finding no material outstanding issues.

The original completion standard is working production panels and bindings, responsive HUD geometry, persistent cable continuations, consistent text/palette/motion and visual review during audio playback, with measured performance and underruns. Future device/session/group actions remain separate work.
