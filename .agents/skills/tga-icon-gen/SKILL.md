---
name: tga-icon-gen
description: Generate pixel-sharp, alpha-correct TGA button/icon textures for Jamma's HUD from an SVG/CSS source, then register them in ResourceList.txt. Use when adding new HUD control art such as the '+' and 'x' trigger buttons. Visual conventions live in a separate, user-editable section below.
---

# Jamma TGA icon generation

This skill is about the **approach**: how to render, downsample, and export a
crisp alpha-correct TGA that behaves under this engine's blend mode. It is not
prescriptive about visual style — see "Visual conventions" below for the
current look, which is a set of editable defaults, not hard rules.

If the user's prompt for a specific icon conflicts with the conventions below
(different border weight, no inner shadow, a different palette, a different
size cap, etc.), the prompt wins for that icon. Only fall back to the
conventions section when the prompt doesn't specify something.

Use the host harness's own tools for each step (browser/screenshot automation
or an SVG rasterizer for rendering, shell for ImageMagick). Nothing here
depends on a specific tool name; substitute whatever the current harness
provides. Create a per-run staging directory under this skill folder (for
example, `.staging/<run-id>/`) and keep all SVG, PNG, and intermediate files
there. Do not write generated intermediates into the skill folder root.

## Engine facts (not editable — verify, don't guess)

- Textures are 32-bit TGA (type 2, uncompressed truecolor + 8-bit alpha),
  origin bottom-left, straight (non-premultiplied) alpha.
- The renderer blends with `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`
  (see `JammaLib/src/graphics/Window.cpp`), so straight-alpha fringing is a
  real risk — see the transparency rules below.
- `ResourceList.txt` declares each texture on its own line as
  `1 <name>` or, for nine-patch backgrounds, `1 <name> ninepatch <borderX> <borderY>`.
  Add new entries there; the loader resolves `textures/<name>.tga` and
  requires `IsNinePatch`/border metadata only for stretchable backgrounds
  (see `JammaLib/src/resources/ResourceLib.cpp`).
- Before drawing, inspect a couple of existing textures in
  `Jamma/resources/textures/` for size/state precedent, e.g.:

  ```powershell
  magick identify -format "%f %wx%h %[colorspace] %A\n" Jamma\resources\textures\rounded_but.tga Jamma\resources\textures\trigger_ditch.tga
  ```

## Visual conventions (editable — treat as current defaults, override freely)

> Edit this section directly as the house style evolves, or override it
> per-icon by describing the desired look in the prompt.

- Border: thick rounded-rect border is the current default weight.
- Shading: a slight inset/inner shadow on the fill is common today, but not
  mandatory — some icons may be flat fill only.
- No text/glyphs for symbolic buttons (e.g. '+' / 'x'); draw shapes as plain
  vector paths instead of rasterized letters.
- Size cap: keep new icons at or under 64x64 unless told otherwise; existing
  precedent: `rounded_but*.tga` 60x60, `trigger_ditch*.tga` 44x44,
  `trigger_back.tga` 64x64, `rounded_rect.tga` 300x108 (ninepatch border
  34x34).
- Multi-state buttons follow a `_over` / `_down` / `_toggled` suffix
  convention (see `rounded_but*.tga`, `stationmode*.tga`); produce the same
  suffix set for a new interactive icon unless the prompt says otherwise.
- Relative sizing: e.g. a '+' (create) control reads larger than an 'x'
  (delete) control at a glance — adjust per prompt if a different hierarchy is
  wanted.

## Transparency rules (avoid straight-alpha fringing — not editable)

There are two different kinds of "transparency" possible in an icon like
this — treat them differently regardless of the chosen visual style:

1. **Any shading applied on top of an opaque fill** (inset shadow or
   otherwise) must be composited onto the opaque base fill color and
   flattened to alpha=255 *before* the outer cutout step. It must never
   remain a semi-transparent layer in the final texture.
2. **The outer silhouette cutout** (rounded-rect corners, any hole in a glyph
   shape) is the only real alpha edge. Because blending here is straight
   (non-premultiplied) alpha, any bilinear filtering or mipmapping will bleed
   whatever RGB sits under a transparent pixel into the visible edge. Prevent
   this by filling the *entire* rendered canvas with the icon's edge/fill
   color first, drawing all shading on top of that full fill, and applying
   the silhouette alpha mask only as the last step. Do not leave transparent
   regions with black/default RGB underneath.

## Pixel-sharp edges vs. curved AA (not editable)

Straight border runs (flat sides between corner radii, or any straight glyph
stroke) must land on exact 0/255 alpha with no blurred half-pixel edge.
Rounded corners will always have a few partial-alpha anti-aliased pixels on
the arc — that's expected and matches existing art.

To get this:

1. Render the SVG/CSS source at an exact integer supersample factor (8x is a
   good default: 480x480 for a 60x60 icon, 512x512 for 64x64).
2. Snap every straight-edge coordinate (stroke rect bounds, corner-radius
   start points) to a multiple of the supersample factor in the supersampled
   coordinate space.
3. Downsample with a **box filter**, not Lanczos/cubic — a box filter on an
   exactly-aligned straight edge produces exact 0/255 alpha; it still
   produces natural AA on the curved corners.

## Pipeline

1. Author the icon as SVG or CSS/HTML at the target final size, following
   whichever conventions apply (defaults above, or the prompt's overrides).
   Fill the whole canvas with the edge/fill color per the transparency rule.
2. Render it to a PNG at `N` times the target size using whatever headless
   rendering capability the harness provides (browser screenshot automation,
   a headless Chrome/Chromium CLI, or an SVG rasterizer such as
   `rsvg-convert`/`resvg`). Keep the alpha channel in the output.
3. Run `render-tga-icon.ps1` (in this skill folder) to box-filter downsample
   and re-merge the alpha channel cleanly. Write the TGA into the same staging
   directory first:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .agents/skills/tga-icon-gen/render-tga-icon.ps1 -InputPng .agents\skills\tga-icon-gen\.staging\<run-id>\trigger_add_8x.png -OutputTga .agents\skills\tga-icon-gen\.staging\<run-id>\trigger_add.tga -TargetSize 64x64
   ```

   The renderer replaces an existing staged output to support iterative icon
   rendering. Keep staged outputs separate from committed texture assets.

4. Repeat per state (`_over`, `_down`, etc.) reusing the same base render
   with only the fill/border color swapped, so all states stay pixel-aligned.
5. Sanity check the staged TGAs with `magick identify -format
   "%f %wx%h %[colorspace] %A\n"` and compare against an existing button of
   the same family; confirm dimensions, `sRGBA` or another RGBA colorspace,
   and an alpha channel (`%A` reports `Blend`/alpha). Inspect the source PNG
   alpha before encoding and inspect representative opaque and edge pixels in
   the staged result. A fully transparent pixel's RGB may be normalized to
   black by ImageMagick during PNG/TGA encoding, so a black RGB value at a
   transparent corner alone does not disprove the source's full-canvas RGB
   fill. The source alpha and the rendered edge behavior are the meaningful
   checks; do not claim that encoded transparent RGB is preserved exactly.
6. Only after explicit final confirmation that the staged TGAs are accepted,
   copy them into `Jamma/resources/textures/` using new, non-existing names.
   Recheck each destination immediately before copying. Copying into the
   required texture directory is the explicit confirmation point and may
   replace an existing texture when iteration is intentional. Only after those
   copies are accepted, add each new file as a line in
   `Jamma/resources/ResourceList.txt` (plain `1 <name>`; these are not
   nine-patch backgrounds). Staging and rendering must not mutate
   `ResourceList.txt`.

## Notes

- If a future icon needs true holes (e.g. a ring), the same full-canvas-fill
  trick applies to every disjoint transparent region, not just the outer
  border.
