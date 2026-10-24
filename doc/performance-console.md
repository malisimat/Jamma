# Performance console

Jamma's command/log window is a native Windows window within `Jamma.exe`.
It needs no companion executable, Windows Terminal profile, or terminal host.
The main window renders its first frame before the console is created. With
`consoleautostart` enabled (the default), the console appears directly behind
Jamma's main window without changing focus or sinking below every other app.
Clicking it later activates it normally.

Set `consoleautostart` to `false` in Jamma's defaults JSON to start with the
console hidden. In the main Jamma window, **Ctrl+`** shows or hides it.
Closing the console also hides it, retaining its transcript and input until
Jamma exits. Jamma continues capturing output while the console is hidden.
The obsolete `consoleforceconhost` preference is ignored.

Type `/`, `/?`, or `/help` for the complete controls reference. General help is
immediate and works offline; it never refreshes the server directory. **F1** in
the console's input or transcript shows the same help and preserves the draft.
The window title keeps a brief help reminder visible.

| Command | Action |
| --- | --- |
| `/`, `/help`, `/?` | General help: commands, keys, modifiers, gestures and controls |
| `/ninjam`, `/n` | List public NINJAM servers and asynchronously refresh live metadata |
| `/connect <number>`, `/c <number>` | Connect using the number in the server list |
| `/disconnect`, `/d`, `/q`, `/quit`, `/exit` | Disconnect from NINJAM; keep Jamma open |
| Text without a leading `/` | Send chat to the current NINJAM server |

Command verbs are case-insensitive. Unknown commands suggest `/help` for controls
and `/ninjam` for servers. Commands execute on Jamma's app thread and use the
current session. Server refresh, connection and chat retain their network behavior.

The transcript uses severity and subsystem colors. Use the scroll bar, mouse
wheel, or native text navigation to browse history. Drag to select text,
double-click to select a word, and press **Ctrl+C** to copy. **Tab** moves
between transcript and input. Incoming output preserves selection and the
position of retained text while browsing; the view follows new output when
already at the bottom without a selection.

The input supports ordinary Windows caret editing, selection, **Ctrl+C**,
and **Ctrl+V**. **Enter** submits a single command. Input is limited to
4096 UTF-8 bytes; invalid text and multiline paste are rejected. Rejected
submissions retain the draft. Console keyboard navigation does not trigger
Jamma's global Insert/Page Up/Page Down performance actions.

The console uses dark grey surfaces and bright input/status text, with a dark
title bar and rounded outer corners where Windows supports them. Standard
window movement, resizing and close controls remain available.
The status row shows NINJAM connection state and the latest event/result.
The layout follows window size and DPI changes. History and capture are
bounded; capture overflow produces a visible loss notice. The transcript keeps
at most 2048 logical entries and 256 Ki UTF-16 units, trimming complete oldest
entries when either limit is reached. At most 32 entries are appended per main
loop iteration so bursts do not cause an unbounded UI update. Restarting Jamma
clears this in-memory history.

## Performance controls reference

The same reference is available through `/help`. Main-window bindings depend on
focus: text entry, popups and the **E** editor consume their contextual input.
In particular, **F1 in the console means help**, while **F1–F9 in the performance
window operate mute groups**. Group shortcuts are disabled during text entry,
dialogs and loop editing. Insert/PageUp/PageDown are also monitored globally
outside the console; the loop editor consumes contextual key input.

| Main-window key | Action |
| --- | --- |
| **Ctrl+`** | Show/hide the console |
| **Ctrl+L** | Load a session with the file picker |
| **Ctrl+S** | Save a numbered session; choose a folder on first Save |
| **Ctrl+Shift+S** | Choose another Save folder; also **Alt-click Save** |
| **Ctrl+Z** | Undo the latest scene/control edit |
| **Tab**, no modifiers | Cycle front, top-down and station-interior camera views |
| **E**, no modifiers | Open the selected completed audio/MIDI loop; hover to disambiguate |
| **Space** | Tap tempo on press; hold the grid until release; works with Ctrl and in the editor, except text entry |
| **Ctrl+Shift+R** | Arm reclock; while following remote timing, choose Stay local first |
| **Ctrl+Shift+V** | Insert a VST on the hovered station/take/loop at the current selection depth |
| **Ctrl+Shift+E** | Open the first VST editor on that hovered target |
| Hold **`** or **numpad 0** | Reveal routing cables |
| **F1–F9** | Recall a mute group, or capture an empty slot |
| **Shift+F1–F9** | Store/overwrite a mute group |
| **Alt+F1–F9** | Clear a mute group |
| **Backspace**, no modifiers | Invert mute on selected takes |
| **PageUp** / **PageDown** | Increase/decrease MIDI channel override, clamped from unchanged through 1–16 |
| Hold **PageUp+PageDown** | Reset MIDI channel override to unchanged |
| Hold **Insert** | Record MIDI/plugin automation while held |
| **Ctrl+Shift+L** | Toggle MIDI CC learn; move the desired CC |
| **Ctrl+Shift+W** | Wire the learned CC to the last touched VST parameter on the targeted MIDI loop's selected automation lane |
| **Ctrl+Shift+X** | Clear that automation lane |
| **Ctrl+Shift+[** / **Ctrl+Shift+]** | Previous/next automation lane, wrapping at the ends |

Mute group creation addresses visible slots and the next add position. On an
empty row, F1 captures the first group; F2–F9 do nothing. Digits continue through
normal routing to configured triggers. On either group row, click recalls or
stores, Shift-click overwrites and Alt-click clears. The **+** button captures
into the first empty slot. Selection groups use mouse controls.

### Selection and mouse/touch gestures

Choose station, take or loop depth in the Selection panel. Left-click selects;
Shift-left-click adds/removes. Left-drag across items paints add/remove based on
the first item's selection state. Middle-click toggles mute and middle-drag
paints that mute/unmute state. Shift-left-click a completed local take/loop sets
it as master when available.

On empty background, left-click clears selection, Alt-left-click selects all
local items at current depth, middle-click unmutes all and Alt-middle-click
mutes all. The last three commit on release without moving; Esc cancels them.
Left-drag pans the camera; right-drag pans with inertia and coast. Wheel zooms
around the pointer, or changes field of view in station-interior view. Primary
touch taps/drags use the corresponding pointer controls supplied by Windows;
there is no separate pinch binding. Loop-editor gestures require a mouse.

Drag mixer faders to change gain. Double-left-click an input/output/overdub,
station/take/loop or monitor fader to reset to **unity (0 dB)**; Ctrl+Z undoes the
reset. A deliberate drag on the second press continues normal dragging.

Rack arrow/router toggles reveal channel faders and routing. In a rack router,
drag between an input and output to toggle their connection. Rack MIDI channel
buttons choose allowed channels independently of the global channel override.

Click panel handles to expand/collapse and Audio/MIDI/Timing/File to switch
settings. Wheel and scroll bars browse overflowing panels. Numeric fields accept
typed values committed with Enter or vertical dragging. Arrows/Home/End move the
caret, Shift selects, and Backspace/Delete remove text. Audio settings choose
device, channels, sample rate and buffer; use the displayed Start/retry controls.
MIDI refresh and Save rig as default are explicit actions.

Station **+** buttons add at either end. Hold Ctrl to reveal station bins, then
click a bin and confirm Delete to remove a local station; at least one remains.
Trigger **+** adds a trigger and **x** asks to delete it. Click input cards to
inspect full identities; Esc closes popups.

Hover sockets/cables or hold **`** to reveal routing. Left-drag a socket or cable
end to a compatible socket to wire/move it. Drop an existing end on empty space
to disconnect. Esc or right-click cancels the cable drag.

### Station VST rack and live MIDI

The Station VST stack sits below its master fader. Click **+** to append an
installed 64-bit VST2/VST3 plugin, or click a loaded slot to toggle its editor.
A failed saved slot remains in place with its state; click it to choose a
replacement for that slot. Cancelling the picker leaves the rack unchanged.
Alt-click a content slot to remove it immediately, including a pending load;
the surviving slots keep their identities and state. The bottom **+** remains.
Long stacks scroll within the rack.
If a plugin cannot report whether it is an instrument or effect, it loads paused.
Shift-click its slot to choose a role; Ctrl-Shift-click returns it to paused
automatic detection. A role change reloads that slot while keeping its saved state.

The channel selectors to the left affect **live MIDI only**. **MIDI − / MIDI +**
collapses or expands them independently of the audio controls. Enabled unwired
channels broadcast to eligible plugins; cables select explicit destinations,
including many channels to one plugin or one channel to several plugins.
Switching a channel off clears its explicit live cables; switching it back on
restores Broadcast. Recorded MIDI keeps its separate output routes and
continues independently of these selectors.

Drag channel sockets or plugin input sockets to connect them. The usual cable
move/disconnect/cancel gestures apply. Hover a socket, cable or collapsed MIDI
button, or hold **`**, to reveal connections. Collapsed and scrolled endpoints
retain their logical identities; expand the channels or scroll the slot into
view to edit its socket. Removing or losing a plugin preserves its explicit
unavailable targets, so a missing plugin never silently enables Broadcast.
Disconnecting the final valid explicit target restores Broadcast; Alt-click a
channel socket to explicitly clear that channel's cables and restore Broadcast.

Station racks sum instruments into the Station audio and process effects in order:
`instrument A, instrument B, reverb` gives `reverb(A + B)`, while
`instrument A, reverb, instrument B` gives `reverb(A) + B`. Take and Loop
effects retain their serial policy.

Each Station supports 32 slots and up to 512 live channel/plugin links.
**Rack info / Rack paused** explains rejected edits or a suspended rack.
Over-limit or malformed imports preserve their saved entries and cables while
inactive. Use the page arrows and Alt-remove unwanted entries, then **Resume**;
**Clear MIDI** explicitly discards all saved live cables after confirmation.
Plugin state and recorded routes remain separate from that recovery action.

### Timing, channel override and triggers

Hold Ctrl outside text entry and the E editor to show **SHFT/DIV** handles, then
drag to change offset/subdivision. Selected targets at the current depth win,
then the hovered target, then global fallback. Targets stay captured during the
gesture. Release the pointer to finish; Esc cancels capture but keeps settings
already applied. See [Overlay controls](overlay-controls.md) for scope details.

Timing **Off** disables MIDI quantisation, **Mixed** uses local choices and
**All** enables it. Subdivision fractions use local grain or the remote NINJAM
beat. Tap tempo and Metronome are separate controls. In a tempo prompt,
**Follow server** accepts remote tempo; **Cancel/Esc** chooses Stay local.

MIDI settings contain **16 channel override toggles (1–16)**. Select one to force
that channel, or click the active toggle off. **All off means unchanged input
channels (pass-through)**. PageUp/PageDown use the same override state.

Activate/Ditch keyboard, MIDI and serial bindings are **configurable in the rig**;
there is no universal record key. Trigger cards provide Activate/Ditch controls.
Activate starts/ends recording. Hold Ditch then Activate to start/end overdub.
During overdub, hold Activate to punch in and release to punch out. Ditch
press/release discards the relevant take/recording state.

### Loop editors

Select a completed, non-empty take/loop and press **E** or its editor button.
Hover disambiguates several selected loops. Esc closes. **Ctrl+Z** undoes and
**Ctrl+Shift+Z** redoes loop edits. Release commits an edit; capture loss or a
changed target cancels unfinished previews.

For MIDI, left-drag paints notes or erases occupied coverage on the active grid.
With quantisation off, click empty space to add, drag a note body to move, or drag
an edge to trim. Right-drag a note changes velocity; right-drag empty MIDI grid
pans pitch vertically and zooms horizontally. Ctrl-left-drag a note moves by grid
cells/semitones; Ctrl-left-drag on empty MIDI grid has no action. Start a left/right
drag off the MIDI grid to orbit. Wheel scrolls pitch and
Shift-wheel zooms it. **CHANNEL** accepts a typed 1–16 plus Enter or vertical
dragging to choose a wired layer. See [Loop grid editor](loop-grid-editor.md).

For the audio spectral surface, left-drag boosts and right-drag cuts.
Alt-left-drag sweeps a broadband band. Wheel changes brush size, Alt-wheel changes
band width, Shift-wheel zooms frequency and Alt-Shift-wheel zooms time.
Shift-left-drag pans time/frequency; Shift-right-drag zooms both axes. Start a
drag off the surface to orbit. Ctrl pointer/wheel gestures are reserved.
