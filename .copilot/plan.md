# Feature: Support for VSTi (VST instruments)

## Summary
Allow loading of VSTi (VST2 and VST3) MIDI instruments on Station objects, which support both realtime play and also playback from recorded MIDI loops.

## Goals & Acceptance Criteria
Allow loading of VSTi (VST2 and VST3) MIDI instruments on Station objects, which support both realtime play and also playback from recorded MIDI loops.  Stations own the instruments because this supports playback before any actual LoopTake or Loop is recorded.  The router (GuiRack) will need to permit routing of different MIDI loop outputs to different VSTi's hosted in Station, so a station could potentially host more than one VSTi and different loops within the station routed individually to each (e.g. to support a kick drum with a sub bass sound from a synth).

## Scope

### In scope
Adjust Station + GuiRack MIDI routing / audio routing.

Live MIDI routing to instruments (should be able to play multiple station's VSTi's with the same MIDI input device simultaneously).

Tests.

### Out of scope
Do not attempt to load an actual VSTi during development.  Tests must not attempt to load a VSTi either.

No changes to store/restore.  No changes to UI.

## Proposed Approach
Break into discrete tasks.  Follow TDD (red, green, refactor).  Rubber duck review once complete.  Generate a summary html doc for review once completed, and open up in browser.

## Risks & Open Questions
Risk of affecting existing VST FX - must not risk breaking existing FX chains.

## TODOs
- [ ] (to be filled by implementing agent)
