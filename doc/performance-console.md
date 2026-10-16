# Performance console

Jamma's command/log window is a native Windows window within `Jamma.exe`.
It needs no companion executable, Windows Terminal profile, or terminal host.
The main window renders its first frame before the console is created. With
`consoleautostart` enabled (the default), the console appears in the background
without changing focus. Clicking it later activates it normally.

Set `consoleautostart` to `false` in Jamma's defaults JSON to start with the
console hidden. In the main Jamma window, **Ctrl+`** shows or hides it.
Closing the console also hides it, retaining its transcript and input until
Jamma exits. Jamma continues capturing output while the console is hidden.
The obsolete `consoleforceconhost` preference is ignored.

Type `/`, `/?`, or `/help` for the server list and command help. `/c <number>`
or `/connect <number>` connects using a number from that list. `/d`, `/q`,
`/quit`, `/exit`, and `/disconnect` disconnect. Lines without a leading slash
are NINJAM chat; unknown slash commands show a hint. **F1** requests help.
Commands execute on Jamma's app thread and use the current session.

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

The status row shows NINJAM connection state and the latest event/result.
The layout follows window size and DPI changes. History and capture are
bounded; capture overflow produces a visible loss notice. The transcript keeps
at most 2048 logical entries and 256 Ki UTF-16 units, trimming complete oldest
entries when either limit is reached. At most 32 entries are appended per main
loop iteration so bursts do not cause an unbounded UI update. Restarting Jamma
clears this in-memory history.
