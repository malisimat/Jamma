# Control Overlay - controls and behaviour

This doc describes the to-be state of the overlay controls/handles that appear when user holds Ctrl button.
The visible handles and what they affect depend on the depth (select) mode, and also the object being hovered at time of Ctrl button first being hit.

## Possible controls/handles
The types of handles/buttons available is currently limited (will be more in future).  These are:

* [blue when global, green when local/selection] Quantisation grid phase offset (affects MIDI quantisation).  Left-Right mouse drag movements.
* [orange when global, red when local/selection] Quantisation grid division factor (grain is subdivided up by varying fractions, adjusted by this control).  Up-Down mouse drag movements.

## Station mode

When depth select mode is set to Station.

### Scenario 1: Stations selected=YES
This does not depend on the object being hovered, selection overrides, and so this behaves identically regardless of where the mouse is at time of Ctrl button being hit.

Handles:
* Phase offset - affects selected stations
* Division factor - affects selected stations

### Scenario 2: Stations selected=NO
This does depend on the object hovered at time of Ctrl first pressed.

If station hovered, handles:
* Phase offset - affects station hovered at time of Ctrl press
* Division factor - affects station hovered at time of Ctrl press

If no station hovered, handles:
* Phase offset - affects global phase
* Division factor - affects all stations

## LoopTake mode

When depth select mode is set to LoopTake.

### Scenario 1: LoopTakes selected=YES
This does not depend on the object being hovered, selection overrides, and so this behaves identically regardless of where the mouse is at time of Ctrl button being hit.

Handles:
* Phase offset - affects selected looptakes (may be across multiple stations)
* Division factor - affects selected looptakes (may be across multiple stations)

### Scenario 2: LoopTakes selected=NO
This does depend on the object hovered at time of Ctrl first pressed.

If looptake hovered, handles:
* Phase offset - affects looptake hovered at time of Ctrl press
* Division factor - affects looptake hovered at time of Ctrl press

If no looptake hovered, handles:
* Phase offset - affects global phase
* Division factor - affects all stations

## Loop mode

When depth select mode is set to Loop.  This actually behaves identically to LoopTake mode, but selection is carried to parent LoopTake rather than affecting individual loops.  So hovering a loop is equivalent to being in LoopTake and hovering the same LoopTake which holds that loop.