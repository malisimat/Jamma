# Feature: quantisation-vis

## Summary
Improve the quantisation 3D visual to match loops, and also fade out

## Goals & Acceptance Criteria
When the quantisation display 'gates' align with each looptake perfectly, and the quantisation display is hidden when not needed, but instantly shows then fades away when needed.  Visual appearance improved significantly, with backing rect, central column and individual looptake-based segments.  Check PR https://github.com/malisimat/Jamma/issues/94

## Scope

### In scope
Quantisation visuals only, no change to other functionality except where it must call a method on the QuantisationModel to indicate visibility, or the number of looptakes and their properties, whatever it needs to disaply correctly.  New shaders are in scope if required.

### Out of scope
anything not related to the visual display or model property updates.

## Proposed Approach
Check issue desscription and specifically ALL COMMENTS on https://github.com/malisimat/Jamma/issues/94

## Risks & Open Questions
risk of slowing visual performance if we go too far

## TODOs
- [ ] (to be filled by implementing agent)
