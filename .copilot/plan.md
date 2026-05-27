# Bugfix: Overdub punch-in records original loop audio to one channel only

## Summary
Overdub loops only record the original looped audio to one channel, whilst live audio during punchin goes correctly to both new loops

## Goals & Acceptance Criteria
Newly created tests pass.

## Scope

### In scope
Fixing the narrow part of code that is currently broken with respect to audio routing.

### Out of scope
Anything that is not explicitly related to the bug.  Do not affect audio timing, UI, etc.

## Proposed Approach
Follow TDD approach (red, green, refactor). Investigate, test hyposthesis using the newly written tests, clean up.  Request rubber duck review at the end.

## Risks & Open Questions
Risk of messing up existing routing (e.g. live audio, or internal loop routing).
Must be extremely careful with respect to the delayed actions - some state changes are delayed to compensate for MaxFadeLoopSamps lead-in.

## TODOs
- [ ] (to be filled by implementing agent)
