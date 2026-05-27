# Feature: MIDI loop quantisation

## Summary
Add per-LoopTake, non-destructive MIDI event start-time quantisation to fractions of the current grain (1, 1/2, 1/4, 1/8, 1/16, 1/32), adjustable in realtime, so playback and 3D visualization reflect quantised timing while preserving original recorded timing.

Reference issue: [#92](https://github.com/malisimat/Jamma/issues/92)

## Goals & Acceptance Criteria
MIDI loop event start times can be quantised to selected fractions of current grain size: 1, 1/2, 1/4, 1/8, 1/16, 1/32.
Quantisation is per-LoopTake and non-destructive (applied on read, not by rewriting stored events).
Quantisation can be adjusted on-the-fly and toggled on/off, returning to original recorded timing with no degradation.
Quantisation moves note start time while preserving note duration (end moves with start).
If shifted note-off passes loop boundary, duration is shortened to fit within loop.
3D MIDI event placement updates in realtime to reflect quantised/unquantised timing changes.
Persistence to JAM file is acknowledged but out of scope for this change.

## Scope

### In scope
Creating a fast, performant 'quantisation transform' applied to a MIDI loop.

Realtime updating visualisation of the quantised MIDI events.

UI method (mouse driven, with modifier maybe?) of enabling/disabling quantisation on a per LoopTake basis, and also mouse driven control of the fraction of the grain.

### Out of scope
Persisting per-LoopTake quantisation settings to JAM files (explicitly beyond this change).
Changes to master loop/grain derivation logic itself.
Broad MIDI editing features beyond quantisation of start timing and duration-preserving shift behavior.

## Proposed Approach
Break into discrete tasks.  Follow TDD (red, green, refactor) for each.  Rubber duck review at the end, focussed on clean surgical code, architecture, thread safety, and realtime hotpath best practice.

For the tasks:
- Implement per-LoopTake quantisation settings (enabled flag + fraction enum/list)
- Create a separate class to handle quantisation transformation, testable
- Apply quantisation to event start-times  non-destructively when reading MIDI events for playback/rendering
- Preserve note duration by shifting end with start; clamp/shorten note-off at loop boundary when needed.
- Wire LoopTake-level UI interaction for realtime toggle/fraction adjustment (issue suggests shift-click drag / shift-click style controls), updating the MIDI event placements as adjustments are made.

## Risks & Open Questions
UI interaction detail is not fully fixed ("shift-click dragging up/down or some other modifier"); exact UX needs confirmation.
Quantisation must stay non-destructive and realtime without introducing timing artifacts between audio/MIDI playback and 3D rendering updates.
Edge handling for note-off clamping at loop boundary needs careful validation for musical feel.
Risk of introducing locks in realtime callbacks (audio, MIDI) or introducing race conditions.
Persistence to JAM is intentionally out of scope now, but this may influence how per-LoopTake quantisation state should be modeled for future compatibility.

## TODOs
- [ ] (to be filled by implementing agent)
