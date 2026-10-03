# Loop grid editor

The loop grid editor opens one completed audio or MIDI loop as a flattened, time-oriented view. Audio loops are view-only. MIDI edits are previewed while dragging, committed when the gesture ends, and included in undo/redo and saved sessions.

## Open and close

- Select a take and one loop, then press **E**. When several loops are selected, hover the loop you want before pressing **E**.
- The editor button also opens the selected loop and becomes **Close editor (Esc)** while it is open.
- Press **Esc** to close. Recording, incomplete, and zero-length loops cannot be opened.

## MIDI editing

- Opening the MIDI editor shows the first wired channel containing events. If every channel is empty, it shows the first wired channel.
- **CHANNEL** is the numeric beside the grid, anchored to its projected boundary as the view rotates. It selects the recorded input layer by its channel number (1–16). Type a number and press **Enter**, or drag the number vertically. Unavailable channels leave the current layer selected and show a message. New notes use the selected layer's input channel. Switching channels cancels an unfinished gesture; each loop retains its undo/redo history.
- **Left-drag** adds notes when starting on an empty cell, or removes note coverage when starting on an occupied cell. With quantisation off, click empty space to create a note; drag a note body to move it, or drag an edge to trim it.
- **Right-drag a note** to change its velocity. Right-drag on empty space orbits the editor view.
- **Ctrl+left-drag a note** moves it by grid cells and semitones when a grid is available. **Ctrl+left-drag empty space** pans and zooms the visible pitch range without changing MIDI.
- **Mouse wheel** scrolls the visible pitch range; **Shift+mouse wheel** zooms it.
- **Ctrl+Z** undoes the last MIDI edit; **Ctrl+Shift+Z** redoes it.

Edits commit once on release. Escape, lost pointer capture, or a changed edit target cancels an in-progress gesture. MIDI events that cannot be mapped unambiguously to source notes are left unchanged.
