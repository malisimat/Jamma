# Loop grid editor

The loop grid editor opens one completed audio or MIDI loop as a flattened, time-oriented view. Audio loops support spectral painting; MIDI loops support note editing. Edits are previewed while dragging, committed when the gesture ends, and included in undo/redo and saved sessions. Type `/help` in the [performance console](performance-console.md) for the complete controls reference.

## Open and close

- Select a take and one loop, then press **E**. When several loops are selected, hover the loop you want before pressing **E**.
- The editor button also opens the selected loop and becomes **Close editor (Esc)** while it is open.
- Press **Esc** to close. Recording, incomplete, and zero-length loops cannot be opened.

## MIDI editing

- At take or station depth, opening the MIDI editor defaults to the first wired channel containing events, or the first wired channel when all are empty. At loop depth, it keeps the selected stream; hover to disambiguate multiple selected streams.
- **CHANNEL** is the numeric beside the grid, anchored to its projected boundary as the view rotates. It selects the recorded input layer by its channel number (1–16). Type a number and press **Enter**, or drag the number vertically. Unavailable channels leave the current layer selected and show a message. New notes use the selected layer's input channel. Switching channels cancels an unfinished gesture; each loop retains its undo/redo history.
- **Left-drag** adds notes when starting on an empty cell, or removes note coverage when starting on an occupied cell. With quantisation off, click empty space to create a note; drag a note body to move it, or drag an edge to trim it.
- **Right-drag a note** to change its velocity. **Right-drag empty MIDI grid** pans pitch vertically and zooms horizontally. Start a left/right drag off the MIDI grid to orbit the editor view.
- **Ctrl+left-drag a note** moves it by grid cells and semitones when a grid is available. **Ctrl+left-drag on empty MIDI grid** has no action.
- **Mouse wheel** scrolls the visible pitch range; **Shift+mouse wheel** zooms it.
- **Ctrl+Z** undoes the last MIDI edit; **Ctrl+Shift+Z** redoes it.

Edits commit once on release. Escape, lost pointer capture, or a changed edit target cancels an in-progress gesture. MIDI events that cannot be mapped unambiguously to source notes are left unchanged.

## Audio spectral editing

On the audio surface, left-drag boosts and right-drag cuts. Alt-left-drag sweeps
a broadband band. Wheel changes brush size, Alt-wheel changes band width,
Shift-wheel zooms frequency and Alt-Shift-wheel zooms time. Shift-left-drag pans
time/frequency; Shift-right-drag zooms both axes. Start a drag off the audio
surface to orbit. Ctrl pointer/wheel gestures are reserved. Ctrl+Z undoes and
Ctrl+Shift+Z redoes audio edits. Editor gestures require a mouse.

## Resolution and tap grids

Timing settings retain straight fractions and add **1/3**, **1/6**, and **1/12**.
The reference is local grain under local timing and remote beat while following
NINJAM; tap-selected base density also composes with the fraction. Boundaries use
rounded rational sample arithmetic rather than accumulating a rounded step.
First and third triplet positions form a 2:1 shuffle; there is no automatic swing.

Space taps work while this editor is open, including with Ctrl held. Text entry
in CHANNEL keeps Space contextual. Space down holds the grid, repeat is ignored,
and release starts its fade. Ctrl moves notes; right-drag on empty MIDI grid controls the pitch view.

Changing resolution projects the original notes onto new onset boundaries while
preserving duration (with existing loop-end clamping). Coarse grids merge visible
coverage and may hide gaps. Resolution alone does not delete, duplicate or rewrite
source events. Returning to the original grid recovers the pattern when no note
edits intervened. Undo/redo tracks source revisions separately and remains valid
across grid changes. A preview using old geometry is cancelled before it can
commit stale coordinates.

Loop-depth selection retains the picked MIDI stream identity. Ctrl loop-scope
settings apply to that stream; take/station settings remain inherited defaults.
The editor, ordinary overlays and playback use the stream's resolved settings.
MIDI source logical lengths remain unchanged when a sole-master tap rounds an
audio boundary; intentional unequal loop periods continue on their own rulers.
