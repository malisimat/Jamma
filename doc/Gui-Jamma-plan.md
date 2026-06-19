# GUI Embellishment Plan

This document is the working handoff for improving Jamma's GUI system. It has been reviewed against the current codebase and corrected to match what actually exists today.

## What exists today

- The GUI is a retained tree of `base::GuiElement` objects, not an immediate-mode UI.
- Rendering and input already flow through the existing object graph:
  - `base::GuiElement` owns child elements, transforms, size, visibility, hit-testing, and default touch handling.
  - Input is routed through `OnAction(actions::KeyAction)`, `OnAction(actions::GuiAction)`, `OnAction(actions::TouchAction)`, and `OnAction(actions::TouchMoveAction)`.
  - `gui::GuiButton` is just a thin `GuiElement` specialization today.
  - `gui::GuiToggle` and `gui::GuiRadio` are the only richer controls in this area.
- Text goes through `graphics::Font`, `resources::ResourceLib::LoadFonts()`, and `gui::GuiLabel`.
- The font path is now stb_truetype-backed: `Inter-Regular.ttf` is loaded from `Jamma/resources/fonts/`, rasterized into a `GL_R8` atlas texture, and drawn with per-glyph quads through `font.vert`/`font.frag`. The old `.txt` metadata files are no longer used.
- `gui::GuiLabel` builds a VAO from `graphics::Font::InitVertexArray()` and draws through `graphics::Font::Draw()`. Font rendering is confirmed working in-app.
- `base::GuiElement` has focus state. `GuiButton` can receive focus and activate on Enter/Space key-up.
- There is a working retained layout system from Phase 2: `gui::GuiGrid` and `gui::GuiStackPanel` support fixed/auto/fill sizing, spacing, padding, and lazy invalidation/recompute.
- Focus is currently per-element (`GuiElement::_hasFocus`) and not yet centralized in a scene-level focus manager.
- There is no existing `GuiScrollBar`, `GuiTextBox`, dropdown, or popup-layer manager in the repo today.

## Implementation guardrails

- Reuse the current architecture. Do not introduce a separate IMGUI system.
- Keep the owning seams stable where practical:
  - `graphics::Font` should remain the text-rendering abstraction boundary.
  - `resources::ResourceLib` should remain the place where fonts are loaded and cached.
  - `gui::GuiLabel` should remain the first consumer of the new text path.
  - `base::GuiElement` should remain the base for input, hit-testing, hierarchy, and sizing.
- Prefer small, testable changes that preserve existing behavior for controls that are not being upgraded.
- For real-time safety, keep GUI-side allocations and rebuilds out of audio paths. GUI allocations are acceptable during resource load, control creation, and explicit text updates.
- At the end of each session, update the relevant phase notes in this file before handing off.
- Agents should complete one full phase at a time, then stop and check in with the user before starting the next phase.

## Recommended owning files

These are the main files future agents should inspect before changing each phase:

- Base GUI/input:
  - `JammaLib/src/base/GuiElement.h`
  - `JammaLib/src/base/GuiElement.cpp`
- Existing controls:
  - `JammaLib/src/gui/GuiButton.h`
  - `JammaLib/src/gui/GuiButton.cpp`
  - `JammaLib/src/gui/GuiToggle.h`
  - `JammaLib/src/gui/GuiToggle.cpp`
  - `JammaLib/src/gui/GuiRadio.h`
  - `JammaLib/src/gui/GuiRadio.cpp`
  - `JammaLib/src/gui/GuiLabel.h`
  - `JammaLib/src/gui/GuiLabel.cpp`
  - `JammaLib/src/gui/GuiGrid.h`
  - `JammaLib/src/gui/GuiGrid.cpp`
- Current font/resource path:
  - `JammaLib/src/graphics/Font.h`
  - `JammaLib/src/graphics/Font.cpp`
  - `JammaLib/src/resources/ResourceLib.h`
  - `JammaLib/src/resources/ResourceLib.cpp`
  - `Jamma/resources/fonts/Inter-Regular.ttf`
  - `Jamma/resources/shaders/font.vert`
  - `Jamma/resources/shaders/font.frag`
- Existing GUI tests:
  - `test/JammaLib_Tests/src/gui/GuiControls_Tests.cpp`
  - `test/JammaLib_Tests/src/gui/GuiLayout_Tests.cpp`
  - `test/JammaLib_Tests/src/gui/GuiSlider_Tests.cpp`
  - `test/JammaLib_Tests/src/gui/GuiRack_Tests.cpp`

## stb notes that matter for this repo

### stb_truetype

- Use the packed-atlas API, not per-frame glyph rasterization:
  - `stbtt_PackBegin`
  - `stbtt_PackSetOversampling`
  - `stbtt_PackFontRanges` or `stbtt_PackFontRangesGatherRects` + `PackRects` + `RenderIntoRects`
  - `stbtt_GetPackedQuad`
- Use `stbtt_InitFont`, `stbtt_ScaleForPixelHeight`, `stbtt_GetFontVMetrics`, and kerning-aware advance logic.
- Prefer glyph/codepoint metrics computed once and reused by `graphics::Font` for measurement and quad generation.
- Oversampling is useful for small UI text. Start conservatively, for example 2x horizontal and 2x vertical, and validate texture size.
- Packed atlases are the right fit here because the current renderer already expects a texture-backed font abstraction and quad-based drawing.
- Keep the loaded font bytes alive for as long as stb font info or repacking depends on them.
- stb does not validate untrusted font files. Only ship trusted bundled font assets.

### stb_textedit

- `stb_textedit` is editing state and command logic only. It does not draw text and does not own the string.
- Integration requires the widget to provide:
  - string length
  - character access
  - insertion/deletion
  - per-row layout
  - per-character width queries
  - key-to-text mapping
- It is suitable for custom widgets in a game-style GUI like this one.
- It works best once Phase 1 has a reliable text measurement and row-layout path.
- Do not introduce `stb_textedit` before there is a clear single-line `GuiTextBox` measurement and cursor-position mapping strategy.

---

## Phase 1 - Core text and focus infrastructure

### Goal

Replace the current atlas-metadata font implementation behind `graphics::Font` with a true font-backed implementation using stb_truetype, then add keyboard focus infrastructure that fits the existing `GuiElement` tree and action dispatch.

### Why this phase is the right seam

- `GuiLabel` is the only current text control, so it is the safest first consumer.
- `graphics::Font` already centralizes string measurement, quad generation, and drawing.
- `ResourceLib::LoadFonts()` already owns font lifetime/caching.
- Buttons already participate in the input system, so focus can be introduced without inventing a separate event layer.

### Required corrections to the old plan

- Do not describe this as "replace existing glyph-based font rendering" at the `GuiLabel` layer only. The replacement belongs primarily in `graphics::Font` and `ResourceLib`.
- Do not use raw `GuiElement*` as the primary focus handle. The existing GUI uses `shared_ptr` ownership and `shared_from_this()`, so focus should be tracked with `std::weak_ptr<base::GuiElement>` or an equivalent safe ownership-aware handle.
- Do not broaden Phase 1 into general text editing. Phase 1 stops at label rendering plus button focus/activation MVP.

### Deliverables

#### 1. Introduce stb_truetype into the repo

- Add `stb_truetype.h` to a suitable third-party location already used by JammaLib conventions.
- Add a single implementation translation unit with `STB_TRUETYPE_IMPLEMENTATION`.
- Keep includes localized and avoid leaking stb macros across unrelated files.

#### 2. Refactor `graphics::Font` to remain the public abstraction

- Preserve the high-level public role of these methods if practical:
  - `InitVertexArray(const std::string&, GLenum, GLuint*, GLuint*)`
  - `Draw(...)`
  - `MeasureString(const std::string&) const`
  - `GetHeight() const`
- Internally replace the old fixed-grid `.txt` width model with:
  - font byte storage
  - `stbtt_fontinfo`
  - atlas texture data
  - packed glyph metrics for the chosen codepoint range(s)
  - ascent/descent/line-gap metrics
- Keep the rendering model quad-based so `GuiLabel` needs minimal change.
- Implement kerning-aware string measurement and quad placement.
- Decide and document the initial supported codepoint range. Minimum acceptable start is printable ASCII plus any codepoints Jamma already depends on, such as space and degree symbol if still needed.

#### 3. Update font resource loading

- Keep `ResourceLib::LoadFonts()` as the entry point for font caching.
- Prefer loading a bundled `.ttf` or `.otf` asset from a repo-controlled path under `Jamma/resources/fonts/`.
- Normalize font path resolution while doing this work. The current code hardcodes `./Resources/Fonts/...`, while the checked-in assets live under `Jamma/resources/fonts/` and other resource types already use dual-path fallback logic in `ResourceLib`.
- Avoid keeping the old dependency on paired `.txt` width metadata once the stb path is live.
- If the old texture atlas path is removed, update any resource assumptions accordingly.
- If a transitional compatibility path is needed, make it explicit and temporary.

#### 4. Upgrade `GuiLabel`

- Keep `GuiLabel` as the first user of the new font path.
- Preserve its current string update model:
  - `SetString()` updates pending text
  - geometry rebuild happens on demand
- Ensure label size and draw behavior continue to scale consistently with current expectations.
- Confirm that the new measurement/rendering path still behaves sensibly for dynamic label text updates.

#### 5. Add keyboard focus infrastructure

- Add a `GuiFocusManager` or equivalent focused owner object that fits retained-mode GUI, not IMGUI.
- Recommended API shape:
  - `bool RequestFocus(const std::shared_ptr<base::GuiElement>& element)`
  - `void ClearFocus()`
  - `bool HasFocus(const std::shared_ptr<base::GuiElement>& element) const`
  - `std::shared_ptr<base::GuiElement> CurrentFocus() const`
- Internally store focus as `std::weak_ptr<base::GuiElement>`.
- Decide where the manager lives before coding. Most likely candidates are the GUI root, scene-level GUI owner, or another long-lived GUI coordinator.
- Add explicit focus eligibility rules. Not every `GuiElement` should automatically become focusable.

#### 6. MVP button keyboard support

- Add the minimum viable keyboard interaction for `GuiButton`-like controls:
  - button can receive focus
  - focused button can activate from keyboard
  - activation should use existing action/result semantics where possible
- Choose and document the activation key set before implementation. Reasonable MVP is Enter and Space on key-up.
- Keep touch behavior unchanged.

### Implementation notes

- The current `graphics::Font` implementation reads only metadata text and generates fixed-grid quads. Future work should preserve the same consumer-facing role while replacing the internals.
- While refactoring font loading, verify and fix success/failure reporting in `ResourceLib::LoadFonts()` instead of carrying forward misleading state.
- The current font code uses only `MeasureString` and `InitVertexArray` for text geometry. Exploit that to limit blast radius.
- `GuiElement::OnAction(KeyAction)` currently only forwards to children. Focus-aware key routing will likely need a focused fast path before or instead of generic child broadcast.
- `GuiElement::OnAction(TouchAction)` already establishes press/release semantics. Use touch-down or touch-up to request focus consistently.
- There are existing GUI control tests, but no current font tests. Add pure CPU tests where possible.

### Tests and validation

- Add new unit tests for `graphics::Font` covering at least:
  - font file load success/failure
  - string measurement
  - kerning-sensitive pairs if the chosen font exposes them
  - codepoint fallback behavior
- Add GUI tests for focus behavior:
  - requesting and clearing focus
  - focused button activation from key input
  - focus transfer on touch/click
- Build at minimum:
  - `Build JammaLib (Debug x64)`
  - `Build Tests (Debug x64)`
- Run the relevant GUI/native tests if practical.

### Exit criteria

- `GuiLabel` renders from a real font atlas generated from stb_truetype.
- `graphics::Font::MeasureString()` is backed by real glyph metrics and kerning, not baked width text files.
- A bundled default font asset is loaded through `ResourceLib`.
- Focus can be assigned, queried, and cleared safely.
- At least one existing button-like control can be activated by keyboard when focused.
- Tests exist for font loading/measurement and focus/button keyboard behavior.

### Phase 1 notes

Mark progress, lessons learned, outstanding work, here:

[x] - Replaced the old metadata-driven font path with a stb_truetype-backed implementation behind graphics::Font and kept the public consumer API intact.
[x] - Added a minimal focus state to GuiElement so controls can request/clear focus and respond to Enter/Space key-up activation.
[x] - Hooked focused button activation through the existing action pipeline, preserving the retained GUI event model instead of introducing a parallel input system.
[x] - Added regression tests for font loading/measurement and focused button keyboard activation.
[x] - Verified the implementation by building JammaLib and the native test target, then running the focused Font/GuiButton tests successfully.
[x] - The repo now prefers the bundled Inter font at Jamma/resources/fonts/Inter-Regular.ttf and only falls back to legacy font names or the OS font path if that asset is unavailable.
[x] - Debugged and fixed font rendering end-to-end. Root cause was a V-flip bug in atlas UV generation: glyphs pack into the top few rows of the atlas (v ≈ 0.004–0.039) and the 1.0f-v flip redirected sampling to the empty bottom of the texture. Fixed by storing atlasVTop/atlasVBottom directly without flipping. Also added glPixelStorei(GL_UNPACK_ALIGNMENT, 1) before atlas upload, a Font destructor to release the atlas GL texture via GlDeleteQueue, and updated font.frag to sample .r from GL_R8 directly. In-app rendering confirmed working.
[x] - Extend the same focus model to additional controls beyond button activation once Phase 2 layout work begins.

**Phase 1 is complete.** All exit criteria met. Proceeding to Phase 2.

---

## Phase 2 - Layout system on top of the existing retained GUI tree

### Goal

Turn the existing `GuiGrid`/panel/container story into a real layout system that sizes and places retained `GuiElement` children responsively.

### Required corrections to the old plan

- Do not start with an immediate-mode `beginHorizontal()` / `endHorizontal()` stack. That does not match the current architecture.
- Build the retained layout primitives first by extending `GuiElement`, `GuiPanel`, and especially `GuiGrid`.
- If convenience builders are desired later, they should wrap the retained system rather than define the core behavior.

### Deliverables

#### 1. Make layout a retained container concern

- Decide whether `GuiGrid` becomes the main layout container or whether a small family of containers is introduced, for example:
  - `GuiStackPanel` / vertical
  - `GuiStackPanel` / horizontal
  - `GuiGrid`
- Reuse `GuiPanel` if it remains a useful generic child-host.
- Keep layout data owned by the container, not recomputed ad hoc by every child.

#### 2. Add sizing contracts to elements

- Add explicit size negotiation to `GuiElement` or a narrow derived interface.
- Minimum suggested surface:
  - `virtual utils::Size2d MinSize() const`
  - optional preferred size
  - optional max size
- Ensure defaults preserve current behavior for controls that are not layout-aware yet.

#### 3. Implement actual `GuiGrid`

- Replace the current stub with working row/column measurement and placement.
- Support:
  - fixed sizes
  - auto/content sizes where practical
  - fill/stretch behavior
  - spacing and padding
  - alignment within cells
- Keep child transforms and local coordinates compatible with existing `GuiElement` drawing and hit-testing.

#### 4. Responsive fallback behavior

- The old plan's "flow down if too narrow" is valid as a behavior goal, but define it precisely.
- Decide whether this is:
  - a wrapping horizontal layout,
  - a grid with responsive column count,
  - or an explicit flow layout container.
- Implement one clear model rather than vague adaptive behavior.

#### 5. Layout invalidation and recompute

- Add a clear invalidation story when:
  - child set changes
  - child min size changes
  - parent size changes
  - text measurement changes after label text updates
- Avoid per-frame unnecessary layout rebuilds.

### Implementation notes

- The current `GuiGrid` is essentially empty; use it as the landing zone rather than creating a competing abstraction.
- Keep layout output expressed through existing `Moveable` and `Sizeable` state.
- Ensure hit-testing remains correct after layout mutates child positions and sizes.
- Text measurement from Phase 1 should become the basis for content-sized labels and later text controls.

### Demo expectation

- Create or update one GUI surface that demonstrates:
  - vertical and horizontal placement
  - grid placement
  - alignment
  - content-sized labels
  - stretch/fill controls
  - at least one responsive narrow-width behavior

### Tests and validation

- Add unit tests for:
  - cell size calculation
  - min-size enforcement
  - wrapping/responsive behavior
  - child rect placement
- Build at minimum:
  - `Build JammaLib (Debug x64)`
  - `Build Tests (Debug x64)`

### Exit criteria

- `GuiGrid` or its replacement is a real retained layout container.
- Child elements can expose at least minimum size.
- Layout recomputes reliably when sizes/content change.
- A demo surface exists using current controls and text.
- Tests cover basic layout math and placement behavior.

### Phase 2 notes

Mark progress, lessons learned, outstanding work, here:

[x] - Added `LayoutSizing` (Fixed/Auto/Fill), `LayoutHAlign`, and `LayoutVAlign` enums to `base::GuiElement.h`; added `ContentSize()`, `GetHorizSizing()`, and `GetVertSizing()` to `GuiElement` with implementations in `GuiElement.cpp`.
[x] - Added `HorizSizing`/`VertSizing` fields to `GuiElementParams` so sizing contracts can be set at construction time alongside existing size fields.
[x] - Replaced the `GuiGrid` stub with a full retained grid-layout container: fixed, auto, and fill track sizing; per-track `minSize` and `spacing`; per-child `GridChildPlacement` with row/col/span and `LayoutHAlign`/`LayoutVAlign` alignment; auto-place sequential children via `AddChild`. Lazy layout recompute via `_layoutDirty` flag.
[x] - Implemented `GuiStackPanel`: vertical and horizontal stacking with spacing, padding, Fill/Auto/Fixed child sizing, and a responsive horizontal-wrap mode that breaks children to a new row when the panel is too narrow. Layout also lazy via `_layoutDirty`.
[x] - Added `ContentSize()` override to `GuiLabel` using `_pendingStr` (not the committed `_str`) so layout reflects the most recent `SetString()` call immediately, before the next `Draw()` syncs the vertex array. Fixed `_stringMutex` to `mutable` for use in `const ContentSize()`.
[x] - Added `GuiStackPanel` to `JammaLib.vcxproj` and `JammaLib.vcxproj.filters`.
[x] - Created a live layout demo panel in `Scene.cpp` (`_layoutDemoPanel`): a vertical `GuiStackPanel` hosting a header label, a horizontal stack with three fill-sized labels (Red/Green/Blue), a 2×2 `GuiGrid` with fixed rows and fill columns, and a wrapping horizontal `GuiStackPanel` with five labels that reflow on resize. The panel is drawn, resource-initialised, and released alongside the existing `_label`.
[x] - Added `test/JammaLib_Tests/src/gui/GuiLayout_Tests.cpp` with 15 tests covering: fixed/fill/auto column/row sizing, padding, min-size floor, spacing, cell origin maths, Fill/Center child alignment, auto-placement, layout invalidation on resize, vertical and horizontal stacking, Fill child stretch, padding inset, horizontal wrap row-break, and equal three-way fill split.
[x] - Both `JammaLib (Debug x64)` and `JammaLib_Tests (Debug x64)` build clean (exit 0). All 31 layout + pre-existing GUI tests pass.
[x] - Ran adversarial review (AgentCouncil / GPT). Critical issues found and fixed: (1) `_placedChildren` deduplication guard added to `AddGridChild` and `AddChild`; (2) `RemoveGridChild` added to `GuiGrid` so both `_children` and `_placedChildren` stay in sync on removal; (3) `_InitResources` in `GuiGrid` and `GuiStackPanel` now calls `GuiPanel::_InitResources` instead of `GuiElement::_InitResources` directly to preserve future vtable dispatch; (4) `Fill` children in `GuiStackPanel` wrap layout now correctly receive the full row height instead of their construction height; (5) `GuiLabel::ContentSize()` now reads `_pendingStr` under the mutex to avoid layout/render string desync after `SetString()`.

**Phase 2 is complete.** All exit criteria met. Proceeding to Phase 3.

---

## Phase 3 - New controls and popup layering

### Goal

Add the next wave of controls only after text, focus, and layout are stable enough to support them cleanly.

### Required corrections to the old plan

- There is no existing `GuiScrollBar` to "incorporate". Scrollbar support is net-new.
- `stb_textedit` should be introduced only for editable text controls, not earlier.
- Popup layering should be treated as infrastructure, not as a side effect of dropdown implementation.

### Suggested implementation order inside Phase 3

1. Popup-layer/input-capture infrastructure
2. Checkbox/toggle cleanup if needed
3. Dropdown/combo box
4. Scrollable panel + scrollbar
5. Single-line `GuiTextBox`
6. Numeric input built on top of `GuiTextBox`
7. Multi-line editing or richer text selection only if still in scope

### Deliverables

#### 1. Popup layers

- Add a way to render some GUI elements after the normal tree.
- Add explicit input capture while a popup is open.
- Define dismissal rules, especially outside click and Escape.
- Define the owning seam and API explicitly before coding (recommended: scene-level popup stack/host that owns open popups and capture target, not ad hoc per-control static state).

#### 1.5 Keyboard routing prerequisite

- Before implementing dropdown/textbox behavior, add explicit key routing for GUI controls.
- Today, `Scene::OnAction(KeyAction)` handles scene shortcuts and station/selector routing but does not route key events through `_guiChildren` / focused GUI controls.
- Add and test an ordered key dispatch policy, for example:
  1. Active popup capture target
  2. Focused GUI control (textbox/dropdown/etc.)
  3. Global scene shortcuts (tap tempo, undo, VST commands) when not text-editing
  4. Existing station/selector fallthrough
- Define shortcut suppression rules while text editing is active so keys like Space/Ctrl+Z/Ctrl+S do not trigger scene-wide behavior unintentionally.

#### 2. Checkbox / toggle

- This may be a thin specialization over the current toggle behavior.
- Ensure it works with both touch and keyboard focus.

#### 3. Dropdown / combo box

- Closed state should behave like a focused button.
- Open state should render through the popup layer.
- Keyboard navigation should at minimum support Up, Down, Enter, Escape.

#### 4. Scrollable container and scrollbar

- Add a scrollable viewport container first.
- Add scrollbar visuals and interaction on top of that container.
- If clipping is needed, evaluate whether current OpenGL draw code already supports scissor state management; otherwise add it centrally.

#### 5. `GuiTextBox` using stb_textedit

- Start with single-line editing unless multi-line is required immediately.
- Integrate `stb_textedit` using the Phase 1 text measurement path for:
  - cursor placement
  - selection extents
  - hit-testing x positions
- Implement:
  - focus-aware keyboard input routing
  - caret display
  - selection highlight
  - delete/backspace
  - home/end
  - clipboard hooks if practical in this codebase
- Be explicit about Unicode scope before coding. Do not imply full Unicode editing support unless actually implemented.

#### 6. Numeric input

- Compose it from text editing plus drag/step behavior.
- Keep validation explicit:
  - min/max
  - step size
  - parse failure behavior
  - commit/cancel behavior

### Implementation notes

- `stb_textedit` requires a real row layout and width query implementation. Reuse the Phase 1/2 text measurement path rather than inventing separate metrics.
- Keep popups and text inputs integrated with one focus authority. Because Phase 1 only added per-element focus state, Phase 3 should either (a) introduce a centralized focus manager now, or (b) introduce an equivalent scene-owned focus coordinator with clear single-owner semantics.
- Avoid introducing control-specific ad hoc key handling when the focus manager can centralize routing.
- Keep drag/capture ownership explicit and single-owner: popup capture, control drag capture, and scene background drag should not compete for the same pointer stream.

### Demo expectation

- Provide a demo panel or screen containing:
  - checkbox
  - dropdown
  - scrollable region
  - text box
  - numeric input
  - popup behavior

### Tests and validation

- Add unit and behavior tests for:
  - popup open/close and input capture
  - key-routing precedence and shortcut suppression while editing text
  - dropdown selection
  - scrollbar range math
  - text editing operations and selection behavior
  - numeric parsing/clamping
- Build at minimum:
  - `Build JammaLib (Debug x64)`
  - `Build Tests (Debug x64)`

### Exit criteria

- Popup infrastructure exists and is used by at least one control.
- Scrollable container and scrollbar behavior are functional.
- `GuiTextBox` supports focused editing through `stb_textedit`.
- Numeric input supports both typing and drag/step adjustment.
- Tests cover the major interaction paths.

### Phase 3 notes

Mark progress, lessons learned, outstanding work, here:

[] -
[] -

---

## Session handoff checklist

Before ending any agent session working on this plan:

1. Update the relevant phase notes above.
2. State clearly whether the phase exit criteria were met.
3. List any files changed.
4. List the exact validation performed.
5. If the phase is incomplete, identify the next concrete starting point, not just a general TODO.

## Next-agent starting point

Phase 1 and Phase 2 are complete. Begin Phase 3 by reviewing:

- `JammaLib/src/gui/GuiGrid.h` / `.cpp` — real layout container (Phase 2)
- `JammaLib/src/gui/GuiStackPanel.h` / `.cpp` — real stack/wrap container (Phase 2)
- `JammaLib/src/base/GuiElement.h` — has `LayoutSizing`, `ContentSize()`, `GetHorizSizing()`, `GetVertSizing()`, `HasFocus()`, `RequestFocus()`, `ClearFocus()`
- `JammaLib/src/gui/GuiButton.h` / `.cpp` — focus-aware, activates on Enter/Space
- `JammaLib/src/gui/GuiPanel.h` / `.cpp` — thin child-host base
- `JammaLib/src/engine/Scene.cpp` — `_layoutDemoPanel` construction plus current UI draw/input ordering (`Draw`, `OnAction(TouchAction)`, `OnAction(TouchMoveAction)`, `OnAction(KeyAction)`)
- `JammaLib/src/graphics/Window.cpp` — current OS-level mouse capture behavior that Phase 3 popup/control capture should align with

Phase 3 starts with popup-layer infrastructure and GUI key-routing infrastructure before any new controls.