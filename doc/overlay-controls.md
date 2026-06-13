# Control Overlay

This document describes the overlay control UI shown while the user holds Ctrl.

The overlay is a direct manipulation layer for quantisation settings. Which handles appear, and what they affect, depends on the current depth selection mode, current selection state, and what the pointer is hovering when Ctrl is first pressed.

## Control Types

The overlay currently exposes two control families:

* Phase offset, shown in blue for global edits and green for selection-local edits. Drag left and right to change the quantisation grid phase.
* Division factor, shown in orange for global edits and red for selection-local edits. Drag up and down to change the quantisation grid division.

These controls are intentionally simple and should remain consistent across Station, LoopTake, and Loop modes.

## Interaction Rules

* Selection always wins over hover when a selection exists for the active depth mode.
* If nothing is selected, the hovered object at the moment Ctrl is first pressed determines the target scope.
* If nothing relevant is hovered, the controls fall back to the global quantisation state.
* The hovered object is sampled when Ctrl is pressed; moving the pointer afterwards should not retarget the active overlay session.

## Station Mode

When depth selection is set to Station:

* If one or more stations are selected, the overlay edits the selected stations regardless of hover state.
* If no station is selected and a station is hovered when Ctrl is pressed, the overlay edits that hovered station.
* If no station is selected and no station is hovered, the overlay edits the global phase and all stations.

### Station Mode Targets

* Phase offset: selected stations, hovered station, or global phase.
* Division factor: selected stations, hovered station, or all stations.

## LoopTake Mode

When depth selection is set to LoopTake:

* If one or more LoopTakes are selected, the overlay edits the selected LoopTakes regardless of hover state.
* If no LoopTake is selected and a LoopTake is hovered when Ctrl is pressed, the overlay edits that hovered LoopTake.
* If no LoopTake is selected and no LoopTake is hovered, the overlay edits the global phase and all stations.

### LoopTake Mode Targets

* Phase offset: selected LoopTakes, hovered LoopTake, or global phase.
* Division factor: selected LoopTakes, hovered LoopTake, or all stations.

## Loop Mode

Loop mode behaves the same as LoopTake mode, except that any loop-level selection is promoted to its parent LoopTake.

In practice, hovering a loop is equivalent to hovering the LoopTake that contains it.

## Behaviour Summary

* Station mode operates on stations.
* LoopTake mode operates on LoopTakes.
* Loop mode reuses LoopTake overlay behavior.
* Selection overrides hover.
* Hover only matters when nothing is selected for the active depth mode.