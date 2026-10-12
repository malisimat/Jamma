# Performance group art

The holder is a single 40px square patch, intended for a 32px button row with
4px padding on each side. Its rounded corners end at 8px. The buttons are
32px square patches with 5px corner extents and 2px borders. The exact slice
coordinates are registered in `ResourceList.txt`; centres and edge runs are
uniform and may stretch horizontally. Keep each row label, numbered label,
plus glyph and last-applied indicator separate from the patches.

`performance_group` is an unfilled outline, including a genuinely transparent
interior. `_on` is a bright opaque fill marking an occupied slot. Both families
have aligned `_over` and `_down` variants. Occupancy does not represent the
last-applied group: the widget supplies that cue independently. Use the tinted
shader for both families and the holder. The add button family is neutral
grayscale and may use opacity for its disabled appearance.

Run `python Jamma/resources/textures/source/performance-groups/render.py` from
the repository root. The script reads these editable SVGs, renders 8x RGB and
silhouette masks separately, invokes the skill's box-filter export script,
and explicitly exports bottom-left, uncompressed 32-bit straight-alpha TGAs.
It validates grayscale channels, sharp straight-edge alpha, curved alpha,
dimensions and exact ninepatch extents. The workspace `.staging/performance-groups`
directory contains all intermediates and a contact sheet showing both tints
and each interactive state. This staging location respects the sandbox's
read-only `.agents` directory.

Add `--install` to copy validated TGAs into the texture directory. Existing
destinations receive numbered `_previous` backups before replacement, as
required by the icon skill. Resource registration is a separate reviewed edit.
