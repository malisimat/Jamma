# Quantisation overlay controls

Hold **Ctrl** to ease the edit panel in (120 ms). Other scene controls fade out
and stop receiving input; release restores them over 420 ms. Rapid modifier
changes continue from the current opacity. Text entry keeps its contextual keys;
the E editor keeps Ctrl move, pan/zoom and undo/redo gestures.

The grey panel has **SHFT** and **DIV** handles. Shift uses blue globally and
green locally; division uses orange globally and red locally. Dragging shows
only the active handle and its caption. The bottom status area temporarily shows
scope, signed shift in ms/samples, or fraction/divisions, including mixed values.
Normal status returns when the gesture ends.
The panel also labels its captured scope: Global, Station, Take or Loop, with
the number of local targets. This label stays fixed while Ctrl holds that capture.

## Scope and capture

At the active selection depth, selected targets win; otherwise the relevant
hovered target wins; otherwise the edit is global. Targets are captured on Ctrl
press and remain stable until that session/gesture ends. Multiple selected
targets are supported. A shift drag applies one absolute offset to the captured
targets, while DIV chooses one fraction; its press feedback reports mixed values.

- Station: SHFT changes station offset; DIV changes the station's current takes.
- Take: SHFT moves every audio and MIDI loop in the captured takes, preserving
  loop-local shifts. DIV changes the captured take settings.
- MIDI loop: SHFT is a stream-local additive offset; DIV is a stream-local
  fraction/enable override. Sibling MIDI streams keep their settings. A missing
  override inherits take settings.
- Audio loop: SHFT delays only the captured audio loops by the displayed samples
  (milliseconds at the current sample rate). Audio playback and waveform move
  together, independently of MIDI grid offsets. DIV remains hidden. Double-click
  SHFT to reset the captured targets to zero. Positive/negative offsets wrap at
  each audio loop's own length; recorded samples are unchanged.
- Global fallback: SHFT changes the global offset; DIV edits current local takes.

The Timing subdivision selection also becomes the default for new local audio
and MIDI takes, and is retained in saved sessions. Selection changes do not alter
that creation default.

Global Off/All still controls playback enablement; Mixed uses take/loop choices.
Global, station, take and stream offsets are composed once. Stream overrides
and local tap base grids are saved separately from the unchanged packed fields.
SHFT moves MIDI playback even with quantisation off, including exact editor
events; it does not enable snapping or rewrite recorded events. Station and
global SHFT also propagate to audio loops through their takes.
With snapping enabled, events and the grid receive the same composed offset;
note-ons snap to the shifted grid and note-offs follow to retain note duration.

Release Ctrl during a drag to retain its capture and grid hold. Mouse release
ends the gesture. Escape, capture/focus loss and deleted targets release the
gesture and temporary feedback. Cancelling a settings drag keeps changes already
published during that drag; unfinished MIDI note previews are discarded.

## Taps and independent grids

**Space down** registers one tap and holds grids; key repeat is ignored. **Space
up** releases the hold without another tap. **Tap tempo (Space)** in Timing uses
the same timestamped engine action and pulses grids. **Metronome** is separate.
Ctrl alone does not hold or pulse grids. Space and handle-drag holds compose, so
releasing one does not hide a grid still held by the other. Focus/session cleanup
clears stale holds.

The first tap starts a sequence without changing subdivisions or their radio.
The second tap must arrive within two seconds. A press-to-press gap of two
seconds or more starts a fresh sequence and discards all previous tap smoothing.
Grids stay fully visible while Space is held and for two seconds after release,
then fade over two seconds once no hold remains. The Timing tap button starts
the same grace period on press. Invalid sample rates or non-increasing timestamps reject that update and restart tap
smoothing; the accepted geometry remains unchanged. Sample zero is valid.

One completed local take counts as one performance, including multichannel and
MIDI-only takes. Remote, incomplete and pre-reclock takes are excluded. With one
eligible take, taps reinterpret its integer beat count. When connected, this
prepares local timing and requests the resulting server BPM/BPI.
Half ties round upward; audio grain floors to integer samples, and logical audio
length is grain × beat count. The physical audio tail remains available and is
retained by session WAV export. MIDI source events and their original logical
length stay intact; if rounding adjusts audio length, those MIDI loops keep
their own period and use the newly published grid.

With additional takes, taps change the base grid only. Master/audio construction
geometry and relative phases stay fixed. Candidates are local grain count (or
remote BPI) × powers of two, with an optional ×3 triplet factor, capped at 256
base divisions. Nearest-count ties choose the smaller candidate. Geometry that
would exceed 8192 cells for a take at 1/32 is rejected with an INFO reason.

The selected base composes once with each take/stream fraction: effective cells
per base interval = base divisions × fraction divisor. Without a tap base, local
fractions refer to local grain; following NINJAM uses the authoritative remote
beat. A tap base adds density relative to that reference. These references need
not have equal lengths. Remote BPM/BPI stays authoritative while following. With additional eligible
takes, local taps choose compatible subdivisions. Reclock suspends following
until the new local master is established and its requested timing is confirmed.
Repeated remote observations retain the tap base; a changed remote geometry or
NoSync resets it. Remote authority is never restored from a saved session.

### Connected NINJAM timing edits

On the Timing page, **Reclock** keeps the NINJAM connection and its remote
metronome active while a new local master is recorded. Once the new master has
activated, Jamma requests its BPM/BPI from the server. Single-master taps also
request updated timing; with more eligible takes they retain subdivision behavior.
Local playback aligns when a fresh server observation confirms both requested
values. A failed request leaves playback local.

Use **Sync to server** to follow current remote timing again after Stay local or
a failed local tempo request. It cancels the old request and waits for fresh
server timing; it may jump local playback into alignment. The adjacent status
shows local, pending, or following state. The metronome continues to reflect the
actual remote beats throughout these workflows.
