# Feature: quantisation

## Summary
Add two features related to quantisation: pressing space bar as a tap tempo, and selecting a master looptake or loop.

## Goals & Acceptance Criteria
When tests pass

## Scope

### In scope
Responding to space bar, responding to selecting a looptake / loop, 3d graphics indicating the quantisation level and master loop length, sending and receiving BPM / BPI changes to/from ninjam

### Out of scope
the change must not affect timing of existing loops. Do not affect the visualisation of existing components like looptakes or loops (only add new visuals)

## Proposed Approach
When space bar is first hit, we record the time, and use subsequent gaps to smooth the time estimate.  This averaged gap estimate is used as the 'seed', to which all loops are quantised (although if it is too short, we still ensure quantising long loop lengths to powers of the master loop).  Gap times are also snapped to whole divisions of the master loop, so the master loop length is a whole multiple of the seed gap.  The gap duration is visualised as a semi trtansparent vertical mesh, like a turngate, where the angle between 'gates' is dictated by the gap size, and the number of 'gates' is equal to the number that make up the master loop.  Visualisation updates in realtime as space bar is pressed.
Clicking a loop or looptake with a special key modifier held, will reset the master loop.  Thererafter, any subsequent loops recorded will then sync to this new master loop.  Also the seed duration will be adapted to fit this new master loop, since the master loop must be a whole multiple of the seed durations.  The visualisation of which master loop should be visible when the modifier is held down, and also some visual indication that the user has clicked a new master loop should be shown.

We should centrally define a common 'timing/synchronisation/bpm/quantisation/bpi' set of calculations that is consistent.  For example, no seeds too short (use global constants to define limits), how to derive bpi from master loop and seed duration, how to convert ninjam bpm into seed duration, etc.  Then wire everything that depends on such calculations to use the same functions.

## Risks & Open Questions
risk of ending up with invalid seed / gap duration.  Risk of inadvertently affecting existing 3D geometry.  Risk of sending invalid BPI / BPM to ninjam.  Risk of incorrectly interpreting ninjam BPM / BPI.  Risk of code bloat.  Keep changes surgical, reuse code where possible.

## TODOs
- [ ] (to be filled by implementing agent)
