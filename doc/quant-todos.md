# Todo's left over from Quantisation phase shift work

Check these are still valid before actioning (code may have changed since these were noted down).

### 1. Overlay semantics are only partially evolved

The overlay gates now use the resolved phase offset, which is good, but there are still visual questions left to settle:

- whether the station overlay should represent the grid only, or grid plus playhead phase
- whether the overlay column/origin marker should remain at loop origin or follow quantisation origin
- whether MIDI note models also need a separate quantisation-origin visual marker instead of only inheriting loop rotation
- how to best represent the quantisation fractions/subdivisions (currently only x1 is displayed, not x0.5, x0.25, etc) for each LoopTake

### 2. No end-to-end integration coverage yet

The branch now has focused unit coverage for snap math and transport-anchor composition, but it still does not prove the full user-facing target behavior:

- two takes with different local zeroes
- two takes in different stations with different local zeroes
- one shared timing seed
- one shared phase origin
- both quantising to the same grid lines

That still needs an integration-style test or a targeted engine test.