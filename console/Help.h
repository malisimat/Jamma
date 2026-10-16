#pragma once

#include "CommandParsing.h"
#include <ostream>
#include <string_view>

namespace console
{
	// App/console owner only. General help is local text: never query the server
	// directory or wait for a Scene, device, connection, or network worker.
	inline constexpr std::string_view GeneralHelpText = R"help([HELP] JAMMA CONTROLS
  Type / or /help for this reference; /? is an alias. Console F1 also shows it.
  Main window Ctrl+` shows/hides the console. Close hides it until reopened.

CONSOLE COMMANDS (case-insensitive verbs)
  /  /help  /?             Show this help immediately, including offline.
  /ninjam  /n              List public NINJAM servers; refresh metadata asynchronously.
  /connect <number>  /c <number>  Connect using a number from /ninjam.
  /disconnect  /d  /q  /quit  /exit  Disconnect from NINJAM (Jamma stays open).
  Text without a leading / is NINJAM chat to the current server.

CONSOLE WINDOW
  Enter submits one line; Tab or Shift+Tab switches transcript/input focus.
  F1 in either text control requests this same help, retaining your draft.
  Input: normal Windows caret/selection editing, Home/End, arrows, Backspace,
    Delete, Ctrl+A/C/X/V/Z; Shift extends selection, Ctrl moves by words.
  Transcript: drag to select; double-click selects a word; Ctrl+C copies.
    Scroll bar, wheel, arrows, PageUp/PageDown, Home/End browse retained text.
  Paste must be one line, at most 4096 UTF-8 bytes. Rejected input keeps the draft.
  Insert/PageUp/PageDown in the console do not fire performance actions.

MAIN WINDOW KEYS
  These actions depend on focus: text fields, dialogs and the E editor consume
    contextual keys. Performance groups are disabled in those contexts.
  Ctrl+L                    Load a session using the file picker.
  Ctrl+S                    Save a numbered session; first Save chooses a folder.
  Ctrl+Shift+S              Choose a different Save folder (also Alt-click Save).
  Ctrl+Z                    Undo the latest scene/control edit.
  Tab (no modifiers)        Cycle front, top-down and station-interior camera views.
  E (no modifiers)          Open one selected completed audio/MIDI loop in the editor.
    Hover to disambiguate multiple selected loops; the editor button also opens it.
  Space                     Tap tempo on press and hold timing grids until release.
    Works with Ctrl and in the editor; typing in a text field keeps Space local.
  Ctrl+Shift+R              Arm reclock; remote timing requires Stay local first.
  Ctrl+Shift+V              Insert a VST on the hovered station/take/loop at current depth.
  Ctrl+Shift+E              Open the first VST editor for that hovered target.
  Hold ` or numeric keypad 0  Reveal routing cables (Ctrl+` toggles console).

SELECTION, MUTE AND PERFORMANCE GROUPS
  Choose station/take/loop depth in the Selection panel; pointer actions use it.
  Left-click an item selects; Shift+left-click adds/removes from selection.
  Left-drag across items paints add/remove, based on the first item's selection.
  Middle-click toggles mute; middle-drag paints that mute/unmute state across items.
  Shift+left-click a completed local loop/take sets it as master when available.
  Empty background: left-click clears selection; Alt+left-click selects all local
    items at current depth; middle-click unmutes all; Alt+middle-click mutes all.
    These background commands commit on release without moving; Esc cancels.
  Mute groups F1-F9 (main window): recall a stored group or capture an empty slot.
    Shift+F1-F9 stores/overwrites; Alt+F1-F9 clears. Only visible slots and the next
    add position can be created: in an empty row F1 stores, F2-F9 do nothing.
  Both group rows: click recalls/stores; Shift+click overwrites; Alt+click clears;
    + captures into the first empty slot. Selection groups use mouse controls.
  Backspace (no modifiers)  Invert mute on selected takes.
  Digits remain available for configured trigger bindings.

POINTER, TOUCH AND MIXING
  Empty-background left-drag pans; right-drag pans with inertia/coast.
    Wheel zooms around the pointer; in station-interior view it changes field of view.
  Primary touch taps/drags use the corresponding pointer controls supplied by Windows.
    There is no separate pinch shortcut; loop-editor gestures below require a mouse.
  Drag mixer faders to change gain. Double-left-click an input/output/overdub,
    station/take/loop or monitor fader to reset to unity (0 dB); Ctrl+Z undoes it.
    A deliberate drag on the second press keeps normal dragging.
  Rack arrow/router toggles reveal channel faders and routing. In a rack router,
    drag between an input and output to toggle their connection. Rack MIDI channel
    buttons choose allowed channels independently of the global channel override.
  Click panel handles to expand/collapse; use Audio/MIDI/Timing/File settings tabs.
    Wheel/scroll bars browse overflowing panels. Click an input card to inspect its
    full identity; Esc closes popups. Trigger + adds; trigger x asks to delete.
  Numeric settings: type and Enter to commit, or drag vertically.
    Arrows/Home/End move the caret; Shift selects; Backspace/Delete remove text.
  Audio settings select device, channels, rate and buffer; use Start/retry as shown.
    MIDI refresh and Save rig as default are explicit settings actions.
  Station + buttons add at either end. Hold Ctrl to reveal station bins; click a bin
    and confirm Delete to remove a local station (at least one must remain).
  Hover sockets/cables or hold ` to reveal routes. Left-drag a socket/cable end
    to a compatible socket to wire/move it; drop an existing end on empty space
    to disconnect. Esc or right-click cancels the cable drag.

TIMING AND MIDI CHANNELS
  Hold Ctrl outside text/E editor to show SHFT/DIV edit handles; drag to change
    offset/subdivision. Selected targets at current depth win, then hovered, then
    global fallback. Targets stay captured during the gesture. Esc cancels capture;
    settings already applied remain. Release the pointer to finish.
  Timing: Off disables MIDI quantisation, Mixed uses local choices, All enables it.
    Subdivision fractions use local grain or the remote NINJAM beat. Tap tempo and
    Metronome are separate controls. Tempo prompts: Follow server applies remote
    tempo; Cancel/Esc chooses Stay local.
  MIDI settings: 16 channel override toggles, 1-16; select one to force that channel.
    Click the active toggle off: all off means unchanged/pass-through input channels.
  PageUp increases override (unchanged -> 1 -> ... -> 16); PageDown decreases it.
    Hold PageUp+PageDown together to reset to unchanged. These and Insert also work
    globally outside the console; the E editor consumes its contextual key input.
  Hold Insert               Record MIDI/plugin automation while held.
  Ctrl+Shift+L              Toggle MIDI CC learn; move the desired CC control.
  Ctrl+Shift+W              Wire learned CC to the last touched VST parameter on
    the targeted MIDI loop's selected automation lane.
  Ctrl+Shift+X              Clear the targeted MIDI loop's selected automation lane.
  Ctrl+Shift+[ / Ctrl+Shift+]  Select the previous/next automation lane (wraps).

CONFIGURABLE PERFORMANCE TRIGGERS
  Activate/Ditch keyboard, MIDI and serial bindings come from the accepted rig;
    there is no universal record key. Trigger cards provide Activate/Ditch controls.
  Activate starts/ends recording. Hold Ditch then Activate to start/end overdub.
    During overdub, hold Activate to punch in; release to punch out.
    Ditch press/release discards the relevant take/recording state.

LOOP EDITOR (E; completed, non-empty loops only)
  Esc closes; Ctrl+Z undoes, Ctrl+Shift+Z redoes the last loop edit.
    Release commits an edit; lost capture/changed target cancels unfinished previews.
  MIDI: left-drag paints notes or erases occupied coverage on the active grid.
    With quantisation off: click empty space adds; drag a note moves; drag an edge trims.
    Right-drag a note changes velocity; right-drag empty space orbits the view.
    Ctrl+left-drag a note moves by grid cells/semitones; Ctrl+left-drag empty space
    pans/zooms pitch (horizontal zoom, vertical pan). Wheel scrolls pitch;
    Shift+wheel zooms pitch. CHANNEL: type 1-16 + Enter or drag to choose a wired layer.
  Audio spectral surface: left-drag boosts, right-drag cuts; Alt+left-drag sweeps
    a broadband band. Wheel changes brush size; Alt+wheel changes band width;
    Shift+wheel zooms frequency; Alt+Shift+wheel zooms time.
    Shift+left-drag pans time/frequency; Shift+right-drag zooms time/frequency.
    Drag off the audio surface to orbit. Ctrl pointer/wheel gestures are reserved.
  Scroll this transcript to revisit controls. Type /ninjam to list servers.
)help";

	inline bool TryWriteGeneralHelp(const ParsedCommand& command, std::ostream& output)
	{
		if (command.Kind != CommandKind::Help) return false;
		output << GeneralHelpText;
		return true;
	}
}
