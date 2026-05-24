# Feature: VST2 support

## Summary
Add VST2 support for VST2 effects, like we already support VST3

## Goals & Acceptance Criteria
Loading a VST2 dll works, and we can open the editor

## Scope

### In scope
VST audio effects (support for Loop, LoopTake, Station - they should just load the corresponding VST version so anywhere we have VST3 we should also support VST2), and also editor window opening/events/closing like VST3 windows.

### Out of scope
MIDI support

## Proposed Approach
Refer to files in @C:\Users\matto\OneDrive\JammaDX\trunk\JammaDX/VST\ and also @C:\Users\matto\OneDrive\JammaDX\trunk\JammaDX\main.cpp for my other project that had working VST2 support.  Directory @C:\Users\matto\VST\64bit/ has a few dll's we can use. Use file extension to determine how to load (VST3 is always .vst3, VST2 is always .dll on Windows).

## Risks & Open Questions
It was complex to ensure we had correct window ownership and lifetime semantics, and smart pointers were not dangling or going out of scope.  Risk of getting this wrong, and not destroying / cleaning up properly.  Risk of not getting window to display, or paint.  Risk of duplicating too much windowing code - we must re-use or extract common code where possible!

## TODOs
- [ ] (to be filled by implementing agent)
