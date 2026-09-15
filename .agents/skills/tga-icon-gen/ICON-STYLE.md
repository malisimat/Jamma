# Jamma icon style guide

User-editable defaults. A specific icon request overrides these values.

## Shape

- Transparent canvas outside the icon silhouette.
- Default: thick rounded-rect border; simpler inset fill.
- Solid border: fully opaque. Fill: opaque or translucent as appropriate.
- Optional: restrained inset shadow or gradient.
- Symbols (`+`, `x`): vector paths, never rasterized text.
- Usual maximum: 64x64. References: `rounded_but*.tga` 60x60;
  `trigger_ditch*.tga` 44x44; `trigger_back.tga` 64x64.

## Colour and shader

- Choose authored colours for the icon's role. Typical GUI orange:
  `glm::vec3(1.0f, 0.7f, 0.2f)` (`#FFB333`).
- `texture_tinted` multiplies texture RGB by `TintColor`; use it only when
  runtime colour variants are wanted, and verify the combined result.
- Hover default: opaque white border and brighter fill. Use an untinted path
  or separate border layer when a tinted shader must not colour that border.

## States

- Normal: chosen base colour.
- Hover: brighter fill; white border by default.
- Down: same silhouette, darker or more inset fill.
- Toggled: clear persistent-state distinction; do not rely on orange alone.
- Toggled Hover: like toggled but white border, brighter fill.
- Toggled Down: like toggled but darker or more inset.
- Out (and Down-Out): Optional extra states, out should be like equivalent hover style, with brightness between normal and hover level.
- State suffixes: `_over`, `_down`, `_toggled` (or established `_on` family).