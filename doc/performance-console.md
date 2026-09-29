# Performance console

Jamma starts `JammaConsole.exe` automatically when `consoleautostart` is true
(the default). The companion opens in a dedicated Windows Terminal window when
`wt.exe` is available. If Terminal is missing or its launch fails, Jamma tries
the built-in Windows Console Host. Terminal is optional; install it from the
[official Windows Terminal instructions](https://learn.microsoft.com/en-us/windows/terminal/install)
if you want its fonts and appearance. A custom Terminal profile is optional;
Jamma passes its own title and command line at launch.
In Windows Terminal, **Ctrl+Shift+mouse wheel** adjusts background opacity
when that [Terminal interaction setting](https://learn.microsoft.com/en-us/windows/terminal/customize-settings/interaction)
is enabled. Optional profile appearance settings include `opacity` and
`useAcrylic`; Jamma does not edit Terminal settings.

Keep `JammaConsole.exe` beside `Jamma.exe` when copying a build. The Jamma
project builds the companion as a dependency into the same configuration output
directory. Copy the runtime DLLs from that directory too, including
`utf8proc.dll`. There is no repository installer or release packaging script.

The startup preference is `consoleautostart` in Jamma's defaults JSON. Set it
to `false` before launching Jamma to leave the companion closed initially.
In the Jamma window, press **Ctrl+Shift+C** to open or reopen it. Repeating the
shortcut while it is connected keeps the same session. If it has closed,
the shortcut starts a fresh companion. The preference controls startup only;
opening the console manually does not change it.

Type `/`, `/?`, or `/help` for the server list and command help. `/c <number>`
or `/connect <number>` connects using a number from that list. `/d`, `/q`,
`/quit`, `/exit`, and `/disconnect` disconnect. Lines without a leading slash
are NINJAM chat; unknown slash commands show a hint.

In the companion, **F1** shows input and selection help. Arrow keys and
Page Up/Down scroll the transcript; **Ctrl+F** returns to the newest entry.
The prompt supports caret and word editing. **Ctrl+V** pastes validated text;
**Ctrl+C** copies selected prompt or transcript text. A left drag selects text
when the terminal delivers mouse reports. Otherwise, use keyboard editing or
the host's native selection. Console Host may render fonts, emoji, and mouse
interaction differently from Windows Terminal.

The companion retains at most 64 MiB and 100,000 logical transcript entries.
Closing and reopening it starts a fresh screen; Jamma replays its last 256
captured output lines from memory, then streams new output. Commands already
executed are not replayed. Restarting Jamma clears that in-memory history.
Status and event loss are reported in the companion when connected.
