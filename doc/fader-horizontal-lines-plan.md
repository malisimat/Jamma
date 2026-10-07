# Dynamic horizontal lines for rack faders

## Scope and current appearance

Add horizontal scale lines to every rack master and channel fader using one
dedicated horizontal line texture drawn repeatedly by code, plus a textured
vertical travel track. The background remains a nine-patch panel with no
baked-in scale. Line count and positions come from
the rendered fader height and the same geometry used to position the handle.

The current handle is 28 px tall and 4 px wider than its background, with
a 2 px overhang on each side. Adjacent channel backgrounds have an 8 px
gap, leaving 4 px clearance between their handles. Background texture opacity is 0.55.
The background fill already has alpha 222/255, so its effective fill opacity
is approximately 0.479 before inherited panel opacity. Handle opacity is
independent. The six existing normal/over/down handle and background textures
retain their 12 by 12 nine-patch insets.

This file is an implementation plan. The dynamic lines and new textures
are not part of the current appearance change.

## Gain scale and mandatory positions

Use the implemented rack scale: **-60 dB through +16 dB**, with silence at
the bottom endpoint and continuous dragging (`Steps == 0`). The GUI uses
`GuiSliderParams::SliderScale::Decibels`; the slider's actual values remain
linear amplitude gain. The maximum has increased slightly from gain 6
(approximately +15.56 dB) to `pow(10.0, 16.0 / 20.0)` (approximately 6.30957).

`AudioMixer::OnAction` passes the slider value to `SetUnmutedLevel`, and the
mixer applies that gain to audio. Therefore **0 dB is gain 1**, not slider
value 0. The bottom endpoint sends gain 0 (negative infinity dB). Any positive
travel uses the finite -60 dB floor, rising exponentially in gain. This
intentional silence detent avoids taking a logarithm of zero. Preserve the
linear gain values passed to the mixer and stored in persistence.

| Position | Gain | Meaning | Line treatment |
| --- | ---: | --- | --- |
| Minimum | 0 | Silence detent at the -60 dB scale endpoint | Major endpoint |
| Intermediate | 0.003981 | -48 dB | Regular major |
| Intermediate | 0.015849 | -36 dB | Regular major |
| Intermediate | 0.063096 | -24 dB | Regular major |
| Intermediate | 0.251189 | -12 dB | Regular major |
| Unity | 1 | 0 dB | Brightest line, distinct warm accent |
| Intermediate | 2.511886 | +6 dB | Regular minor |
| Intermediate | 3.981072 | +12 dB | Regular major |
| Maximum | 6.309573 | +16 dB | Major endpoint |

Generate the full base grid at **6 dB intervals**, so all grid positions are
evenly spaced in decibels and handle travel, with +16 dB added as an endpoint
after the +12 dB grid mark. The table above lists examples, not the complete
grid. If labels are added later, use `20 * log10(gain)` for
positive gains and display silence separately for 0. No text labels are
required for the first implementation.

Intermediate horizontal lines are narrower and less opaque than the silence,
unity and maximum lines. Major and minor intermediate lines share one width;
minor lines have lower opacity. Beneath the horizontal marks and handle, draw
a 4 px wide black vertical track with rounded caps, centred on the handle
travel axis and spanning its minimum and maximum centre positions.

## Position calculation

Implement a pure layout helper owned by `GuiSlider`, for example
`BuildScaleMarks(params, size)`, returning records with integer centre Y,
gain, and kind (`Endpoint`, `Unity`, `Major`, `Minor`). Enable this scale only
for rack faders through an explicit parameter; generic horizontal panel
sliders must keep their existing appearance.

For a vertical fader:

```cpp
const auto travel = CalcDragLength(params, size);
const auto handleHeight = params.DragControlSize.Height;
const auto position = CalcDragPos(params, size, gain);
const auto centreY = position.Y + handleHeight / 2;
```

Use `CalcDragPos` directly, rather than reproducing or reversing its mapping.
It calls the implemented `ValueToFraction` logarithmic mapping, rounds the
normalized travel fraction and includes
`DragControlOffset.Y`. The line runs through the handle centre at that gain,
not through the handle top or the panel edge. Use local GUI coordinates and
the slider's existing MVP translation; no additional screen-Y inversion.

With the current geometry and a background height H, normal travel is
`H - 28 - 2*4 = H - 36` and centre positions range from 18 to `H - 18`.
Unity is `18 + round(travel * 60 / 76)`. Check the actual returned layout rather
than assuming these constants if handle geometry changes.

Reject degenerate input before calling the existing mapping: nonpositive
value range or a panel height no greater than handle height plus both gaps.
The implemented `CalcDragLength` returns zero when the handle and gaps do
not fit, and dragging then preserves the gain. In that case draw no marks;
do not manufacture a travel range or send any audio action.

## Height-dependent count and spacing

Use 6 dB base intervals from -60 through +12 dB, then add the +16 dB
endpoint. The final interval is 4 dB; do not redistribute the grid across
76 dB, which would shift unity and the other meaningful gain marks.
When more height exists, subdivide the 6 dB intervals equally and continue
that finer spacing past +12 dB towards the maximum. Keep +16 dB as an exact
endpoint, truncating the last interval if needed. Use real arithmetic for
spacing and dB calculations:

```text
targetSpacing = 14 pixels
subdivisions = clamp(floor(6 * travel / (76 * targetSpacing)), 1, 4)
lastGridIndex = floor(76 * subdivisions / 6)
decibels(i) = -60 + 6 * i / subdivisions, i = 0..lastGridIndex
gain(i) = i == 0 ? 0 : pow(10, decibels(i) / 20)
```

Append the maximum endpoint if the grid does not already reach +16 dB.
This produces 14 marks at ordinary heights, 27 when travel reaches 355 px,
39 at 532 px, and 52 at 710 px, before pixel-row deduplication. The final
interval may be shorter than the regular spacing. Classify unity by index
`i == 10 * subdivisions`, endpoints by their boundary positions, and other
multiples of 12 dB as major (`i % (2 * subdivisions) == 0`); all remaining
marks are minor. Use integer grid indices for classification, rather than
floating-point dB equality. The handle stays continuous: do not change
`Steps` to the number of marks.

For short faders with travel below 114 px (less than 6 px for the final
4 dB interval of the base grid), show only the three mandatory anchors:
silence, 0 dB, and +16 dB. Omit additional marks rather than crowding a
small control.

Round through `CalcDragPos`, then deduplicate coincident pixel rows with
priority Unity, Endpoint, Major, Minor. Pixel spacing may differ by one pixel
after rounding; that is expected. For extremely short travel that cannot
give all mandatory anchors distinct rows, show only the noncolliding anchors
and enforce a larger minimum layout height in the rack before claiming full
scale support. Never move the 0 dB line away from gain 1 to make space.

If the range changes later, update the grid policy explicitly: unity remains
gain 1, the lower finite scale bound comes from `MinDecibels`, the lower
detent from `Min`, and maximum dB from `20 * log10(Max)`. Do not silently
assume the current grid indices or thresholds for a different range.
`ValueToFraction` and `FractionToValue` must remain the single shared mapping
for input and layout.

## Dedicated line texture

Generate `Jamma/resources/textures/fader_scale_line.tga` with tga-icon-gen.
Use a 32 by 4 px canvas, a 2 px visible horizontal stroke centred in the
middle two rows, transparent top/bottom guards, and 2 px rounded end caps.
The stretchable centre must be uniform horizontally. RGB beneath clear and
partially transparent pixels must match the adjoining stroke colour so
straight-alpha filtering does not produce dark fringes.

Author a warm near-white stroke with a subtle vertical highlight. Render at
8x and box-downsample; verify uncompressed 32-bit type-2 TGA, bottom-left
origin, straight alpha, crisp straight edges, and cap geometry. Register:

```text
1 fader_scale_line ninepatch 3 0
```

The X inset preserves both caps while stretching the middle; the Y inset is
zero because every draw stays exactly 4 px high. Minimum rendered width is
6 px. Reuse this one texture for every line and every state. Use the existing
`texture_tinted` shader with explicitly supplied tint and per-draw opacity;
do not inherit the default orange tint or the background's 0.55 multiplier.

Starting appearance values (insets apply on each side of the panel):

| Kind | Horizontal inset | Opacity | Tint |
| --- | ---: | ---: | --- |
| Unity | 8 px | 0.90 | Warm amber `#FFC878` |
| Endpoint | 8 px | 0.65 | Pale rose `#F6DDE6` |
| Major | 12 px | 0.45 | Pale rose `#F6DDE6` |
| Minor | 12 px | 0.25 | Pale rose `#F6DDE6` |

Clamp line width to the panel's inner bounds; skip lines that cannot fit the
two caps. The image's local Y is `centreY - 2`, making the visible stroke
straddle the handle centre. Snap X, Y and width to pixels. The uniform centre
stretches horizontally; caps and stroke thickness remain fixed.

## Vertical track texture

Generate `Jamma/resources/textures/fader_scale_track.tga` with tga-icon-gen
and register it as `fader_scale_track`. Use a vertical nine-patch with a
uniform black centre and preserved rounded caps; stretch only along Y.
Its visible width is 4 px, independent of height. Use transparent guards
and the same straight-alpha/filtering checks as the horizontal texture.
Choose canvas dimensions and insets to preserve the caps without stretching
them. Draw with explicit black tint and full opacity before inherited slider
opacity; do not apply the background's 0.55 multiplier.

Place the track on the handle's centre X using the existing drag geometry.
Its visible outer ends coincide with the silence and maximum handle centre
positions. Snap its bounds to pixels and omit it when travel is too short
to fit both caps or the layout is degenerate. Cache its geometry alongside
the marks; short faders showing only anchors still show the track when it fits.

## Rendering and resource lifetime

Add four retained `graphics::Image` instances to `GuiSlider`, one for each
mark kind, one retained track image, and cached CPU mark records. The four
mark images reference `fader_scale_line`; the track image references
`fader_scale_track`. Initialize and release the images alongside
`_dragElement` in `_InitResources` and `_ReleaseResources`. All faders share
the named textures through `ResourceLib`; do not allocate an image or VAO for
each individual line.

Draw in this order inside `GuiSlider::Draw`:

1. Existing `GuiElement::Draw` draws the panel in its current state.
2. Push the existing slider-position translation.
3. Draw the vertical track, then the cached marks using the retained image
   for each kind. Push a translation for each mark; set tint; scope opacity
   with `WithOpacity`; draw; pop.
4. Draw `_dragElement`.
5. Draw the existing VU overlay and pop the slider translation.

Update each kind's image width only when the panel width changes. Its
fixed-height nine-patch geometry is reused for every line of that kind;
there is no `SetSize` call inside the repeated drawing loop and no repeated
buffer upload merely because two consecutive marks have different widths.
Resize the track image only when its cached bounds change. Restore tint
after the track and line pass, and use scoped opacity so subsequent handle
and VU drawing receive their existing values. The translucent handle may
reveal a subtle line underneath its fill, which is intentional.

Cache layout by panel size, handle size, drag offset, gaps, orientation,
range, and scale-enabled flag. Invalidate from `SetSize`, `SetDragParams`,
and any future range setter. Rebuild mark records when that key changes;
reuse capacity and perform no allocation or geometry construction per frame.
Keep all GL initialization, buffer updates and deletion in the established
render resource lifecycle. The audio callback does no scale work.

## Implementation files and validation

- `JammaLib/src/gui/GuiSlider.h`: opt-in scale parameter, mark types, pure
  layout helper, retained mark/track images and layout cache.
- `JammaLib/src/gui/GuiSlider.cpp`: shared gain-to-position calculation,
  invalidation, init/release and the draw pass before the handle.
- `JammaLib/src/gui/GuiRack.cpp`: enable the scale in `_GetSliderParams`,
  which serves master and dynamically created channel sliders alike.
- `Jamma/resources/ResourceList.txt`: register both textures. The existing
  `CopyJammaResources` target in `Jamma/Jamma.vcxproj` deploys the textures and list
  on incremental builds.
- `test/JammaLib_Tests/src/gui/GuiSlider_Tests.cpp`: verify anchors coincide
  with actual handle centres at silence, gain 1 and maximum; uniform dB
  subdivisions and the final partial interval; count thresholds; rounding;
  short/zero travel; duplicate row priority; track bounds; resizing and no
  slider quantization changes.

Use GPU render evidence to check normal/over/down master and channel faders
on short, ordinary and tall backgrounds, with a handle at minimum, unity and
maximum. Check caps, horizontal widths, track width and extent, tint/opacity
restoration, VU visibility, and line placement after resize. Confirm resource-only builds deploy the
new textures and modified list without relinking the executable.

Relevant existing code: `GuiRack::_GetSliderParams`,
`GuiSlider::CalcDragPos`, `GuiSlider::CalcDragLength`, `GuiSlider::Draw`,
`AudioMixer::OnAction`, `AudioMixer::WriteBlock`, and
`graphics::Image::Draw`. Preserve their subsystem ownership and existing
real-time constraints throughout the implementation.
